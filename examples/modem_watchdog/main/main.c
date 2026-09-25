/**
 * @file main.c
 * @brief Modem watchdog + MicroLink Tailscale subnet router for classic ESP32.
 *
 * Hardware:
 *   GPIO27 HIGH -> relay released -> NC closed -> modem powered
 *   GPIO27 LOW  -> relay energized -> NC open   -> modem power cut
 *
 * The ESP32 itself must be powered from the 12V input BEFORE the relay.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "microlink.h"

static const char *TAG = "modem_watchdog";

/* ============================================================================
 * Hardware / watchdog policy
 * ========================================================================== */

#define RELAY_GPIO                 GPIO_NUM_27
#define RELAY_MODEM_ON_LEVEL       1
#define RELAY_MODEM_OFF_LEVEL      0

#define CHECK_INTERVAL_MS          (60ULL * 1000ULL)
#define FAILURES_BEFORE_REBOOT     5
#define MODEM_OFF_MS               (20ULL * 1000ULL)
#define MODEM_BOOT_MS              (5ULL * 60ULL * 1000ULL)
#define MAX_AUTO_REBOOTS           3
#define REBOOT_WINDOW_MS           (2ULL * 60ULL * 60ULL * 1000ULL)
#define TCP_PROBE_TIMEOUT_MS       2500

typedef enum {
    WD_NORMAL = 0,
    WD_POWER_CUT,
    WD_WAITING_FOR_MODEM,
} watchdog_state_t;

static volatile watchdog_state_t wd_state = WD_NORMAL;
static volatile bool manual_reboot_requested = false;
static volatile int consecutive_failures = 0;
static volatile int automatic_reboots = 0;

static uint64_t state_started_ms = 0;
static uint64_t last_internet_check_ms = 0;
static uint64_t reboot_window_started_ms = 0;

/* ============================================================================
 * WiFi / MicroLink state
 * ========================================================================== */

static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static microlink_t *ml = NULL;
static httpd_handle_t http_server = NULL;

static inline uint64_t now_ms(void) {
    return (uint64_t)(esp_timer_get_time() / 1000);
}

/* ============================================================================
 * Relay
 * ========================================================================== */

static void relay_init_safe(void) {
    /* Load HIGH into the output latch before switching the pin to output.
     * This minimizes an active-low pulse during application startup.
     *
     * For a truly fail-safe appliance, also add an external ~10k pull-up from
     * relay IN to 3V3 so the relay stays released during reset/boot ROM time. */
    gpio_set_pull_mode(RELAY_GPIO, GPIO_PULLUP_ONLY);
    gpio_set_level(RELAY_GPIO, RELAY_MODEM_ON_LEVEL);
    ESP_ERROR_CHECK(gpio_set_direction(RELAY_GPIO, GPIO_MODE_OUTPUT));
    gpio_set_level(RELAY_GPIO, RELAY_MODEM_ON_LEVEL);

    ESP_LOGI(TAG, "Relay safe state: modem ON");
}

static void modem_on(void) {
    gpio_set_level(RELAY_GPIO, RELAY_MODEM_ON_LEVEL);
    ESP_LOGW(TAG, "RELAY OFF -> NC closed -> modem ON");
}

static void modem_off(void) {
    gpio_set_level(RELAY_GPIO, RELAY_MODEM_OFF_LEVEL);
    ESP_LOGW(TAG, "RELAY ON -> NC open -> modem OFF");
}

/* ============================================================================
 * Internet probes
 * ========================================================================== */

static bool tcp_probe(const char *ip, uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return false;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        return false;
    }

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };
    if (inet_pton(AF_INET, ip, &dest.sin_addr) != 1) {
        close(fd);
        return false;
    }

    int rc = connect(fd, (struct sockaddr *)&dest, sizeof(dest));
    if (rc == 0) {
        close(fd);
        return true;
    }

    if (errno != EINPROGRESS) {
        close(fd);
        return false;
    }

    fd_set writefds;
    FD_ZERO(&writefds);
    FD_SET(fd, &writefds);

    struct timeval tv = {
        .tv_sec = TCP_PROBE_TIMEOUT_MS / 1000,
        .tv_usec = (TCP_PROBE_TIMEOUT_MS % 1000) * 1000,
    };

    rc = select(fd + 1, NULL, &writefds, NULL, &tv);
    if (rc <= 0) {
        close(fd);
        return false;
    }

    int so_error = 0;
    socklen_t len = sizeof(so_error);
    rc = getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len);
    close(fd);

    return rc == 0 && so_error == 0;
}

static bool internet_available(void) {
    if ((xEventGroupGetBits(wifi_event_group) & WIFI_CONNECTED_BIT) == 0) {
        ESP_LOGW(TAG, "Internet check: WiFi is disconnected");
        return false;
    }

    if (tcp_probe("1.1.1.1", 443)) return true;
    if (tcp_probe("8.8.8.8", 53)) return true;
    if (tcp_probe("9.9.9.9", 53)) return true;

    return false;
}

/* ============================================================================
 * Watchdog state machine
 * ========================================================================== */

static const char *watchdog_state_name(watchdog_state_t state) {
    switch (state) {
        case WD_NORMAL:            return "monitoring";
        case WD_POWER_CUT:         return "modem-off";
        case WD_WAITING_FOR_MODEM: return "waiting-for-modem";
        default:                   return "unknown";
    }
}

static void refresh_reboot_window(uint64_t now) {
    if (now - reboot_window_started_ms >= REBOOT_WINDOW_MS) {
        reboot_window_started_ms = now;
        automatic_reboots = 0;
        ESP_LOGI(TAG, "Automatic reboot window reset");
    }
}

static bool start_modem_reboot(bool manual) {
    if (wd_state != WD_NORMAL) {
        return false;
    }

    uint64_t now = now_ms();

    if (!manual) {
        refresh_reboot_window(now);
        if (automatic_reboots >= MAX_AUTO_REBOOTS) {
            ESP_LOGE(TAG, "Automatic reboot limit reached (%d/%d)",
                     automatic_reboots, MAX_AUTO_REBOOTS);
            return false;
        }
        automatic_reboots++;
    }

    consecutive_failures = 0;
    modem_off();
    wd_state = WD_POWER_CUT;
    state_started_ms = now;

    ESP_LOGW(TAG, "Modem reboot started (%s)", manual ? "manual" : "automatic");
    return true;
}

static void update_reboot_state(uint64_t now) {
    if (wd_state == WD_POWER_CUT && now - state_started_ms >= MODEM_OFF_MS) {
        modem_on();
        wd_state = WD_WAITING_FOR_MODEM;
        state_started_ms = now;
        ESP_LOGW(TAG, "Modem powered back on; waiting %llu seconds",
                 (unsigned long long)(MODEM_BOOT_MS / 1000ULL));
        return;
    }

    if (wd_state == WD_WAITING_FOR_MODEM && now - state_started_ms >= MODEM_BOOT_MS) {
        wd_state = WD_NORMAL;
        consecutive_failures = 0;
        last_internet_check_ms = now;
        esp_wifi_connect();
        ESP_LOGI(TAG, "Modem boot grace period finished; monitoring resumed");
    }
}

static void watchdog_task(void *arg) {
    reboot_window_started_ms = now_ms();
    last_internet_check_ms = now_ms();

    while (1) {
        uint64_t now = now_ms();

        if (manual_reboot_requested) {
            manual_reboot_requested = false;
            start_modem_reboot(true);
        }

        update_reboot_state(now);

        if (wd_state == WD_NORMAL && now - last_internet_check_ms >= CHECK_INTERVAL_MS) {
            last_internet_check_ms = now;
            refresh_reboot_window(now);

            ESP_LOGI(TAG, "Checking internet...");
            if (internet_available()) {
                consecutive_failures = 0;
                ESP_LOGI(TAG, "Internet OK");
            } else {
                consecutive_failures++;
                ESP_LOGW(TAG, "Internet failure %d/%d",
                         consecutive_failures, FAILURES_BEFORE_REBOOT);

                if (consecutive_failures >= FAILURES_BEFORE_REBOOT) {
                    start_modem_reboot(false);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

/* ============================================================================
 * HTTP UI
 * ========================================================================== */

static esp_err_t root_handler(httpd_req_t *req) {
    char vpn_ip[16] = "not-connected";
    if (ml) {
        uint32_t ip = microlink_get_vpn_ip(ml);
        if (ip) microlink_ip_to_str(ip, vpn_ip);
    }

    const bool wifi_ok =
        (xEventGroupGetBits(wifi_event_group) & WIFI_CONNECTED_BIT) != 0;

    char html[2048];
    int n = snprintf(
        html, sizeof(html),
        "<!doctype html><html><head>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Modem Watchdog</title>"
        "<style>"
        "body{font-family:-apple-system,BlinkMacSystemFont,Arial,sans-serif;"
        "background:#f4f4f4;margin:0;padding:24px}"
        ".c{max-width:540px;margin:auto;background:#fff;padding:24px;"
        "border-radius:16px;box-shadow:0 3px 16px #0001}"
        ".r{padding:7px 0;border-bottom:1px solid #eee}"
        "button{width:100%%;margin-top:20px;padding:14px;border:0;"
        "border-radius:10px;font-size:16px;cursor:pointer}"
        "</style></head><body><div class='c'>"
        "<h2>Modem Watchdog</h2>"
        "<div class='r'><b>State:</b> %s</div>"
        "<div class='r'><b>WiFi:</b> %s</div>"
        "<div class='r'><b>Tailscale IP:</b> %s</div>"
        "<div class='r'><b>Failures:</b> %d/%d</div>"
        "<div class='r'><b>Auto reboots:</b> %d/%d</div>"
        "<form method='POST' action='/reboot'>"
        "<button type='submit'>Reboot modem now</button></form>"
        "</div></body></html>",
        watchdog_state_name(wd_state),
        wifi_ok ? "connected" : "disconnected",
        vpn_ip,
        consecutive_failures, FAILURES_BEFORE_REBOOT,
        automatic_reboots, MAX_AUTO_REBOOTS);

    if (n < 0) {
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t health_handler(httpd_req_t *req) {
    char vpn_ip[16] = "";
    if (ml) {
        uint32_t ip = microlink_get_vpn_ip(ml);
        if (ip) microlink_ip_to_str(ip, vpn_ip);
    }

    const bool wifi_ok =
        (xEventGroupGetBits(wifi_event_group) & WIFI_CONNECTED_BIT) != 0;

    char json[384];
    snprintf(json, sizeof(json),
             "{\"state\":\"%s\",\"wifi\":%s,\"tailscale_connected\":%s,"
             "\"tailscale_ip\":\"%s\",\"failures\":%d,\"auto_reboots\":%d}",
             watchdog_state_name(wd_state),
             wifi_ok ? "true" : "false",
             (ml && microlink_is_connected(ml)) ? "true" : "false",
             vpn_ip,
             consecutive_failures,
             automatic_reboots);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t reboot_handler(httpd_req_t *req) {
    if (wd_state != WD_NORMAL) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "Modem reboot already in progress.\n");
    }

    manual_reboot_requested = true;
    return httpd_resp_sendstr(req, "Reboot requested. Power will be cut for 20 seconds.\n");
}

static void start_http_server(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 4096;
    config.max_uri_handlers = 8;

    ESP_ERROR_CHECK(httpd_start(&http_server, &config));

    const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_handler,
    };
    const httpd_uri_t health = {
        .uri = "/health",
        .method = HTTP_GET,
        .handler = health_handler,
    };
    const httpd_uri_t reboot = {
        .uri = "/reboot",
        .method = HTTP_POST,
        .handler = reboot_handler,
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &health));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &reboot));

    ESP_LOGI(TAG, "Watchdog HTTP UI listening on port 80");
}

/* ============================================================================
 * WiFi
 * ========================================================================== */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data) {
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "WiFi disconnected; reconnecting...");
        esp_wifi_connect();
        return;
    }

    if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);

        ESP_LOGI(TAG, "WiFi got IP: " IPSTR, IP2STR(&event->ip_info.ip));

        /* The modem reboot changes the underlay while the ESP stays powered.
         * Rebind keeps the same Tailscale identity/session and rebuilds the
         * sockets once WiFi returns. */
        if (ml) {
            ESP_LOGI(TAG, "WiFi returned; rebinding MicroLink");
            esp_err_t err = microlink_rebind(ml);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "microlink_rebind failed: %s", esp_err_to_name(err));
            }
        }
    }
}

static void wifi_init(void) {
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char *)wifi_config.sta.ssid,
            CONFIG_ML_WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password,
            CONFIG_ML_WIFI_PASSWORD, sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "WiFi started; SSID=%s", CONFIG_ML_WIFI_SSID);
}

/* ============================================================================
 * MicroLink
 * ========================================================================== */

static void on_microlink_state(microlink_t *handle, microlink_state_t state,
                               void *user_data) {
    ESP_LOGI(TAG, "MicroLink state=%d", (int)state);

    if (state == ML_STATE_CONNECTED) {
        char ip[16];
        microlink_ip_to_str(microlink_get_vpn_ip(handle), ip);
        ESP_LOGI(TAG, "Tailscale connected: %s", ip);
#ifdef CONFIG_ML_ENABLE_SUBNET_ROUTER
        if (CONFIG_ML_SUBNET_ROUTE[0] != '\0') {
            ESP_LOGI(TAG, "Advertising subnet route: %s", CONFIG_ML_SUBNET_ROUTE);
        }
#endif
    }
}

static void microlink_task(void *arg) {
    /* The watchdog is already running while we wait. A dead modem at boot
     * therefore cannot strand the appliance before the relay logic starts. */
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);

    microlink_config_t config = {
        .auth_key = CONFIG_ML_TAILSCALE_AUTH_KEY,
        .device_name = CONFIG_ML_DEVICE_NAME,
        .enable_derp = true,
        .enable_stun = true,
        .enable_disco = true,
        .max_peers = CONFIG_ML_MAX_PEERS,
        .wifi_tx_power_dbm = 0,
#ifdef CONFIG_ML_ENABLE_SUBNET_ROUTER
        .advertise_route = CONFIG_ML_SUBNET_ROUTE,
#endif
    };

    ml = microlink_init(&config);
    if (!ml) {
        ESP_LOGE(TAG, "microlink_init failed");
        vTaskDelete(NULL);
        return;
    }

    microlink_set_state_callback(ml, on_microlink_state, NULL);

    esp_err_t err = microlink_start(ml);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "microlink_start failed: %s", esp_err_to_name(err));
    }

    vTaskDelete(NULL);
}

/* ============================================================================
 * Main
 * ========================================================================== */

void app_main(void) {
    /* Protect modem power before doing anything slow. */
    relay_init_safe();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Starting modem watchdog + MicroLink subnet router");

    wifi_init();
    start_http_server();

    /* Watchdog must not depend on MicroLink being online. */
    BaseType_t ok = xTaskCreate(
        watchdog_task, "watchdog", 6144, NULL, 6, NULL);
    ESP_ERROR_CHECK(ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    ok = xTaskCreate(
        microlink_task, "microlink_boot", 8192, NULL, 5, NULL);
    ESP_ERROR_CHECK(ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
