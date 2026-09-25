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
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"

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

#define DEFAULT_CHECK_INTERVAL_S      60U
#define DEFAULT_FAILURES_BEFORE_REBOOT 5U
#define DEFAULT_MODEM_OFF_S            20U
#define DEFAULT_MODEM_BOOT_S           300U
#define DEFAULT_MAX_AUTO_REBOOTS       3U
#define DEFAULT_REBOOT_WINDOW_S        7200U
#define TCP_PROBE_TIMEOUT_MS           2500

#define APP_CONFIG_NAMESPACE           "modem_cfg"
#define APP_CONFIG_KEY                 "settings"
#define APP_CONFIG_VERSION             1

typedef struct {
    uint8_t version;

    char wifi_ssid[33];
    char wifi_pass[65];
    char prev_wifi_ssid[33];
    char prev_wifi_pass[65];
    uint8_t wifi_pending;

    char auth_key[96];
    char device_name[48];

    char ctrl_host[64];
    uint8_t ctrl_tls;
    char noise_pubkey_hex[65];

    uint8_t subnet_enabled;
    char subnet_route[24];
    char priority_peer_ip[16];

    uint32_t check_interval_s;
    uint8_t failures_before_reboot;
    uint16_t modem_off_s;
    uint16_t modem_boot_s;
    uint8_t max_auto_reboots;
    uint32_t reboot_window_s;
} app_settings_t;

static app_settings_t app_cfg;

static void copy_str(char *dst, size_t dst_len, const char *src) {
    if (!dst || dst_len == 0) return;
    snprintf(dst, dst_len, "%s", src ? src : "");
}

static void app_config_defaults(app_settings_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->version = APP_CONFIG_VERSION;

    copy_str(cfg->wifi_ssid, sizeof(cfg->wifi_ssid), CONFIG_ML_WIFI_SSID);
    copy_str(cfg->wifi_pass, sizeof(cfg->wifi_pass), CONFIG_ML_WIFI_PASSWORD);
    copy_str(cfg->auth_key, sizeof(cfg->auth_key), CONFIG_ML_TAILSCALE_AUTH_KEY);
    copy_str(cfg->device_name, sizeof(cfg->device_name), CONFIG_ML_DEVICE_NAME);
    copy_str(cfg->ctrl_host, sizeof(cfg->ctrl_host), CONFIG_ML_CTRL_HOST);
#ifdef CONFIG_ML_CTRL_TLS
    cfg->ctrl_tls = 1;
#else
    cfg->ctrl_tls = 0;
#endif
#ifdef CONFIG_ML_CTRL_NOISE_PUBKEY_HEX
    copy_str(cfg->noise_pubkey_hex, sizeof(cfg->noise_pubkey_hex),
             CONFIG_ML_CTRL_NOISE_PUBKEY_HEX);
#endif
#ifdef CONFIG_ML_ENABLE_SUBNET_ROUTER
    cfg->subnet_enabled = CONFIG_ML_SUBNET_ROUTE[0] != '\0';
    copy_str(cfg->subnet_route, sizeof(cfg->subnet_route), CONFIG_ML_SUBNET_ROUTE);
#else
    cfg->subnet_enabled = 0;
#endif
#ifdef CONFIG_ML_PRIORITY_PEER_IP
    copy_str(cfg->priority_peer_ip, sizeof(cfg->priority_peer_ip),
             CONFIG_ML_PRIORITY_PEER_IP);
#endif

    cfg->check_interval_s = DEFAULT_CHECK_INTERVAL_S;
    cfg->failures_before_reboot = DEFAULT_FAILURES_BEFORE_REBOOT;
    cfg->modem_off_s = DEFAULT_MODEM_OFF_S;
    cfg->modem_boot_s = DEFAULT_MODEM_BOOT_S;
    cfg->max_auto_reboots = DEFAULT_MAX_AUTO_REBOOTS;
    cfg->reboot_window_s = DEFAULT_REBOOT_WINDOW_S;
}

static esp_err_t app_config_save(void) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(APP_CONFIG_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    err = nvs_set_blob(nvs, APP_CONFIG_KEY, &app_cfg, sizeof(app_cfg));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static void app_config_load(void) {
    app_config_defaults(&app_cfg);

    nvs_handle_t nvs;
    if (nvs_open(APP_CONFIG_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        ESP_LOGI(TAG, "No runtime settings yet; using sdkconfig defaults");
        return;
    }

    app_settings_t stored = {0};
    size_t len = sizeof(stored);
    esp_err_t err = nvs_get_blob(nvs, APP_CONFIG_KEY, &stored, &len);
    nvs_close(nvs);

    if (err == ESP_OK && len == sizeof(stored) &&
        stored.version == APP_CONFIG_VERSION) {
        app_cfg = stored;
        ESP_LOGI(TAG, "Runtime settings loaded from NVS");
    } else {
        ESP_LOGI(TAG, "No compatible runtime settings; using sdkconfig defaults");
    }
}

static inline uint64_t cfg_check_interval_ms(void) {
    return (uint64_t)app_cfg.check_interval_s * 1000ULL;
}

static inline uint64_t cfg_modem_off_ms(void) {
    return (uint64_t)app_cfg.modem_off_s * 1000ULL;
}

static inline uint64_t cfg_modem_boot_ms(void) {
    return (uint64_t)app_cfg.modem_boot_s * 1000ULL;
}

static inline uint64_t cfg_reboot_window_ms(void) {
    return (uint64_t)app_cfg.reboot_window_s * 1000ULL;
}

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
static char local_ip[16] = "";

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

static const char *watchdog_state_label(watchdog_state_t state) {
    switch (state) {
        case WD_NORMAL:            return "monitorando";
        case WD_POWER_CUT:         return "fonte desligada";
        case WD_WAITING_FOR_MODEM: return "aguardando modem";
        default:                   return "desconhecido";
    }
}

static void refresh_reboot_window(uint64_t now) {
    if (now - reboot_window_started_ms >= cfg_reboot_window_ms()) {
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
        if (automatic_reboots >= app_cfg.max_auto_reboots) {
            ESP_LOGE(TAG, "Automatic reboot limit reached (%d/%d)",
                     automatic_reboots, app_cfg.max_auto_reboots);
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
    if (wd_state == WD_POWER_CUT && now - state_started_ms >= cfg_modem_off_ms()) {
        modem_on();
        wd_state = WD_WAITING_FOR_MODEM;
        state_started_ms = now;
        ESP_LOGW(TAG, "Modem powered back on; waiting %llu seconds",
                 (unsigned long long)(cfg_modem_boot_ms() / 1000ULL));
        return;
    }

    if (wd_state == WD_WAITING_FOR_MODEM && now - state_started_ms >= cfg_modem_boot_ms()) {
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

        if (wd_state == WD_NORMAL && now - last_internet_check_ms >= cfg_check_interval_ms()) {
            last_internet_check_ms = now;
            refresh_reboot_window(now);

            ESP_LOGI(TAG, "Checking internet...");
            if (internet_available()) {
                consecutive_failures = 0;
                ESP_LOGI(TAG, "Internet OK");
            } else {
                consecutive_failures++;
                ESP_LOGW(TAG, "Internet failure %d/%d",
                         consecutive_failures, app_cfg.failures_before_reboot);

                if (consecutive_failures >= app_cfg.failures_before_reboot) {
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

/* Settings are deliberately served by this app instead of MicroLink's
 * optional config HTTP server so port 80 stays owned by one lightweight
 * httpd instance on the RAM-constrained WROOM. */
static bool valid_hex64(const char *s) {
    if (!s || s[0] == '\0') return true;
    if (strlen(s) != 64) return false;
    for (size_t i = 0; i < 64; i++) {
        char ch = s[i];
        if (!((ch >= '0' && ch <= '9') ||
              (ch >= 'a' && ch <= 'f') ||
              (ch >= 'A' && ch <= 'F'))) {
            return false;
        }
    }
    return true;
}

static bool valid_ipv4_string(const char *s) {
    if (!s || !s[0]) return false;
    struct in_addr addr;
    return inet_pton(AF_INET, s, &addr) == 1;
}

static bool valid_ipv4_cidr(const char *s) {
    if (!s || !s[0]) return false;
    char ip[16];
    unsigned prefix = 0;
    char extra = '\0';
    if (sscanf(s, "%15[^/]/%u%c", ip, &prefix, &extra) != 2) return false;
    return prefix >= 1 && prefix <= 32 && valid_ipv4_string(ip);
}

static char *read_http_body(httpd_req_t *req, size_t max_len) {
    if (!req || req->content_len <= 0 || (size_t)req->content_len > max_len) {
        return NULL;
    }
    char *buf = malloc((size_t)req->content_len + 1);
    if (!buf) return NULL;

    int got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, buf + got, req->content_len - got);
        if (n <= 0) {
            free(buf);
            return NULL;
        }
        got += n;
    }
    buf[got] = '\0';
    return buf;
}

static esp_err_t send_json_obj(httpd_req_t *req, cJSON *json) {
    httpd_resp_set_hdr(req, "Connection", "close");
    if (!json) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    char *body = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!body) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_sendstr(req, body);
    free(body);
    return ret;
}

static esp_err_t send_json_error(httpd_req_t *req, const char *status,
                                 const char *message) {
    httpd_resp_set_status(req, status);
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "ok", false);
    cJSON_AddStringToObject(json, "error", message ? message : "invalid request");
    return send_json_obj(req, json);
}

static void delayed_restart_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static esp_err_t settings_api_get_handler(httpd_req_t *req) {
    cJSON *json = cJSON_CreateObject();
    if (!json) return ESP_FAIL;

    cJSON_AddStringToObject(json, "wifi_ssid", app_cfg.wifi_ssid);
    cJSON_AddBoolToObject(json, "wifi_password_configured", app_cfg.wifi_pass[0] != '\0');
    cJSON_AddBoolToObject(json, "wifi_pending", app_cfg.wifi_pending != 0);

    cJSON_AddStringToObject(json, "device_name", app_cfg.device_name);
    cJSON_AddBoolToObject(json, "auth_key_configured", app_cfg.auth_key[0] != '\0');

    cJSON_AddStringToObject(json, "ctrl_host", app_cfg.ctrl_host);
    cJSON_AddBoolToObject(json, "ctrl_tls", app_cfg.ctrl_tls != 0);
#ifdef CONFIG_ML_CTRL_TLS
    cJSON_AddBoolToObject(json, "tls_supported", true);
#else
    cJSON_AddBoolToObject(json, "tls_supported", false);
#endif
    cJSON_AddStringToObject(json, "noise_pubkey", app_cfg.noise_pubkey_hex);

    cJSON_AddBoolToObject(json, "subnet_enabled", app_cfg.subnet_enabled != 0);
#ifdef CONFIG_ML_ENABLE_SUBNET_ROUTER
    cJSON_AddBoolToObject(json, "subnet_supported", true);
#else
    cJSON_AddBoolToObject(json, "subnet_supported", false);
#endif
    cJSON_AddStringToObject(json, "subnet_route", app_cfg.subnet_route);
    cJSON_AddStringToObject(json, "priority_peer_ip", app_cfg.priority_peer_ip);

    cJSON_AddNumberToObject(json, "check_interval_s", app_cfg.check_interval_s);
    cJSON_AddNumberToObject(json, "failures_before_reboot", app_cfg.failures_before_reboot);
    cJSON_AddNumberToObject(json, "modem_off_s", app_cfg.modem_off_s);
    cJSON_AddNumberToObject(json, "modem_boot_s", app_cfg.modem_boot_s);
    cJSON_AddNumberToObject(json, "max_auto_reboots", app_cfg.max_auto_reboots);
    cJSON_AddNumberToObject(json, "reboot_window_s", app_cfg.reboot_window_s);

    return send_json_obj(req, json);
}

static bool json_copy_optional_string(cJSON *root, const char *name,
                                      char *dst, size_t dst_len,
                                      bool empty_means_keep) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!item) return true;
    if (!cJSON_IsString(item) || !item->valuestring) return false;
    if (empty_means_keep && item->valuestring[0] == '\0') return true;
    if (strlen(item->valuestring) >= dst_len) return false;
    copy_str(dst, dst_len, item->valuestring);
    return true;
}

static bool json_uint_in_range(cJSON *root, const char *name,
                               uint32_t min, uint32_t max, uint32_t *out) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsNumber(item)) return false;
    double d = item->valuedouble;
    if (d < (double)min || d > (double)max || d != (double)(uint32_t)d) {
        return false;
    }
    *out = (uint32_t)d;
    return true;
}

static esp_err_t settings_api_post_handler(httpd_req_t *req) {
    char *body = read_http_body(req, 4096);
    if (!body) {
        return send_json_error(req, "400 Bad Request", "invalid or empty request body");
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        return send_json_error(req, "400 Bad Request", "invalid JSON");
    }

    app_settings_t next = app_cfg;

    cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "wifi_ssid");
    if (!cJSON_IsString(ssid) || !ssid->valuestring ||
        ssid->valuestring[0] == '\0' || strlen(ssid->valuestring) > 32) {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request", "Wi-Fi SSID must be 1-32 characters");
    }

    cJSON *wifi_pass = cJSON_GetObjectItemCaseSensitive(root, "wifi_password");
    if (wifi_pass && (!cJSON_IsString(wifi_pass) || !wifi_pass->valuestring ||
                      strlen(wifi_pass->valuestring) > 64)) {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request", "Wi-Fi password is too long");
    }

    bool wifi_changed = strcmp(ssid->valuestring, app_cfg.wifi_ssid) != 0;
    if (wifi_pass && wifi_pass->valuestring[0] != '\0' &&
        strcmp(wifi_pass->valuestring, app_cfg.wifi_pass) != 0) {
        wifi_changed = true;
    }

    if (wifi_changed) {
        copy_str(next.prev_wifi_ssid, sizeof(next.prev_wifi_ssid), app_cfg.wifi_ssid);
        copy_str(next.prev_wifi_pass, sizeof(next.prev_wifi_pass), app_cfg.wifi_pass);
        next.wifi_pending = 1;
        copy_str(next.wifi_ssid, sizeof(next.wifi_ssid), ssid->valuestring);
        if (wifi_pass && wifi_pass->valuestring[0] != '\0') {
            copy_str(next.wifi_pass, sizeof(next.wifi_pass), wifi_pass->valuestring);
        }
    }

    if (!json_copy_optional_string(root, "device_name",
                                   next.device_name, sizeof(next.device_name), false) ||
        !json_copy_optional_string(root, "auth_key",
                                   next.auth_key, sizeof(next.auth_key), true) ||
        !json_copy_optional_string(root, "ctrl_host",
                                   next.ctrl_host, sizeof(next.ctrl_host), false) ||
        !json_copy_optional_string(root, "noise_pubkey",
                                   next.noise_pubkey_hex, sizeof(next.noise_pubkey_hex), false) ||
        !json_copy_optional_string(root, "subnet_route",
                                   next.subnet_route, sizeof(next.subnet_route), false) ||
        !json_copy_optional_string(root, "priority_peer_ip",
                                   next.priority_peer_ip, sizeof(next.priority_peer_ip), false)) {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request", "one or more text fields are invalid");
    }

    if (next.ctrl_host[0] == '\0' || strstr(next.ctrl_host, "://") ||
        strchr(next.ctrl_host, '/') || strchr(next.ctrl_host, ':')) {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request", "control-plane host must be a hostname only");
    }
    if (!valid_hex64(next.noise_pubkey_hex)) {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request", "Noise public key must be 64 hex characters");
    }
    if (next.priority_peer_ip[0] && !valid_ipv4_string(next.priority_peer_ip)) {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request", "priority peer must be an IPv4 address");
    }

    cJSON *tls = cJSON_GetObjectItemCaseSensitive(root, "ctrl_tls");
    if (!cJSON_IsBool(tls)) {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request", "ctrl_tls must be boolean");
    }
#ifdef CONFIG_ML_CTRL_TLS
    next.ctrl_tls = cJSON_IsTrue(tls) ? 1 : 0;
#else
    if (cJSON_IsTrue(tls)) {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request", "TLS support is not compiled into this firmware");
    }
    next.ctrl_tls = 0;
#endif

    cJSON *subnet = cJSON_GetObjectItemCaseSensitive(root, "subnet_enabled");
    if (!cJSON_IsBool(subnet)) {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request", "subnet_enabled must be boolean");
    }
#ifdef CONFIG_ML_ENABLE_SUBNET_ROUTER
    next.subnet_enabled = cJSON_IsTrue(subnet) ? 1 : 0;
#else
    if (cJSON_IsTrue(subnet)) {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request", "subnet routing is not compiled into this firmware");
    }
    next.subnet_enabled = 0;
#endif
    if (next.subnet_enabled && !valid_ipv4_cidr(next.subnet_route)) {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request", "subnet route must be an IPv4 CIDR");
    }

    uint32_t v = 0;
    if (!json_uint_in_range(root, "check_interval_s", 5, 3600, &v)) {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request", "check interval must be 5-3600 seconds");
    }
    next.check_interval_s = v;
    if (!json_uint_in_range(root, "failures_before_reboot", 1, 20, &v)) {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request", "failure threshold must be 1-20");
    }
    next.failures_before_reboot = (uint8_t)v;
    if (!json_uint_in_range(root, "modem_off_s", 1, 120, &v)) {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request", "power-off duration must be 1-120 seconds");
    }
    next.modem_off_s = (uint16_t)v;
    if (!json_uint_in_range(root, "modem_boot_s", 10, 1800, &v)) {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request", "boot grace period must be 10-1800 seconds");
    }
    next.modem_boot_s = (uint16_t)v;
    if (!json_uint_in_range(root, "max_auto_reboots", 1, 20, &v)) {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request", "automatic reboot limit must be 1-20");
    }
    next.max_auto_reboots = (uint8_t)v;
    if (!json_uint_in_range(root, "reboot_window_s", 60, 86400, &v)) {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request", "reboot window must be 60-86400 seconds");
    }
    next.reboot_window_s = v;

    cJSON_Delete(root);

    app_cfg = next;
    esp_err_t err = app_config_save();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to persist settings: %s", esp_err_to_name(err));
        return send_json_error(req, "500 Internal Server Error", "failed to persist settings");
    }

    ESP_LOGI(TAG, "Runtime settings saved%s", wifi_changed ? " (Wi-Fi pending validation)" : "");

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddBoolToObject(resp, "restart", true);
    cJSON_AddBoolToObject(resp, "wifi_changed", wifi_changed);
    esp_err_t ret = send_json_obj(req, resp);

    BaseType_t ok = xTaskCreate(delayed_restart_task, "settings_restart",
                                2048, NULL, 8, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Could not schedule settings restart");
    }
    return ret;
}

static const char DASHBOARD_HTML[] =
    "<!doctype html>\n"
    "<html lang='pt-BR'>\n"
    "<head>\n"
    "<meta charset='utf-8'>\n"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>\n"
    "<meta name='color-scheme' content='dark'>\n"
    "<title>Modem Watchdog</title>\n"
    "<style>\n"
    ":root{\n"
    "  --bg:#07111e;--panel:#0d1a2b;--line:#203650;--line2:#294765;\n"
    "  --text:#edf4ff;--muted:#93a8c4;--blue:#438cff;--green:#27c45b;\n"
    "  --red:#f14955;--amber:#f4b942;--shadow:0 16px 40px #0006;\n"
    "  --pad:clamp(8px,1vw,18px);--gap:clamp(7px,.8vw,13px);\n"
    "}\n"
    "*{box-sizing:border-box}\n"
    "html,body{margin:0;width:100%;height:100%;overflow:hidden;background:radial-gradient(circle at 16% 0,#0d2641 0,transparent 28%),linear-gradient(180deg,#07111e,#06101b 70%);color:var(--text);font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif}\n"
    "button,input{font:inherit}button{cursor:pointer}\n"
    ".app{width:100vw;height:100dvh;padding:var(--pad);display:grid;grid-template-rows:auto auto minmax(0,1fr);gap:var(--gap);overflow:hidden}\n"
    ".hero{display:flex;align-items:center;justify-content:space-between;gap:12px;min-width:0}\n"
    ".brand{display:flex;align-items:center;gap:clamp(9px,1vw,14px);min-width:0}\n"
    ".brandmark{width:clamp(36px,4.2vh,48px);height:clamp(36px,4.2vh,48px);color:#4595ff;display:grid;place-items:center;flex:0 0 auto}\n"
    ".brandmark svg{width:100%;height:100%}\n"
    ".hero h1{font-size:clamp(18px,2.5vh,28px);line-height:1;margin:0;font-weight:800;letter-spacing:-.02em;white-space:nowrap}\n"
    ".hero p{margin:4px 0 0;color:var(--muted);font-size:clamp(10px,1.45vh,14px);white-space:nowrap}\n"
    ".hero-actions{display:flex;align-items:center;justify-content:flex-end;gap:clamp(6px,.7vw,10px);min-width:0}\n"
    ".badge{display:none;align-items:center;gap:6px;height:clamp(34px,4.8vh,44px);padding:0 clamp(10px,1vw,16px);border-radius:999px;background:linear-gradient(180deg,#f04d61,#db2d48);font-weight:800;font-size:clamp(10px,1.35vh,13px);box-shadow:0 8px 24px #e23b5040;white-space:nowrap}\n"
    ".badge.show{display:flex}.badge svg{width:17px;height:17px}\n"
    ".health-link{height:clamp(34px,4.8vh,44px);display:flex;align-items:center;padding:0 clamp(9px,.9vw,14px);border:1px solid var(--line2);border-radius:10px;color:#79b8ff;text-decoration:none;font-size:clamp(10px,1.35vh,13px);white-space:nowrap}\n"
    ".btn{height:clamp(34px,4.8vh,44px);padding:0 clamp(12px,1.2vw,22px);border-radius:10px;border:0;color:white;font-weight:800;font-size:clamp(10px,1.45vh,14px);white-space:nowrap}\n"
    ".btn:disabled{opacity:.48;cursor:not-allowed}\n"
    ".btn-primary{background:linear-gradient(180deg,#4c96ff,#2f79f6);box-shadow:0 8px 22px #387fe438}\n"
    ".btn-danger{background:linear-gradient(180deg,#fb5960,#ed3949);box-shadow:0 8px 22px #e8404c32}\n"
    "\n"
    ".notice{display:none;padding:7px 11px;border:1px solid #715c22;border-radius:10px;background:#2a2413;color:#f4d579;font-size:11px}.notice.show{display:block;position:absolute;left:var(--pad);right:var(--pad);top:calc(var(--pad) + 52px);z-index:20}\n"
    ".stats{display:grid;grid-template-columns:repeat(7,minmax(0,1fr));gap:var(--gap);min-width:0}\n"
    ".stat,.panel{background:linear-gradient(180deg,#102036cc,#0a1727ee);border:1px solid var(--line);box-shadow:var(--shadow)}\n"
    ".stat{min-width:0;height:clamp(58px,8.2vh,78px);border-radius:13px;padding:clamp(7px,1vh,12px);display:flex;gap:clamp(7px,.75vw,11px);align-items:center}\n"
    ".stat-icon,.panel-icon{color:#6ca9ff;flex:0 0 auto}.stat-icon{width:clamp(24px,3.4vh,31px);height:clamp(24px,3.4vh,31px);display:grid;place-items:center}.stat-icon svg{width:100%;height:100%}\n"
    ".stat-label{color:#a9bad1;font-size:clamp(9px,1.25vh,12px);margin-bottom:2px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}\n"
    ".stat-value{font-weight:800;font-size:clamp(11px,1.65vh,15px);line-height:1.15;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}\n"
    ".dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px;vertical-align:-1px}.dot.ok{background:var(--green);box-shadow:0 0 8px #27c45b70}.dot.bad{background:var(--red);box-shadow:0 0 8px #f1495570}.state-warn{color:#ffd36a}\n"
    "\n"
    "#form{min-height:0;height:100%}\n"
    ".panels{height:100%;display:grid;grid-template-columns:1.28fr 1fr .78fr 1.22fr;gap:var(--gap);min-width:0;min-height:0}\n"
    ".panel{min-width:0;min-height:0;border-radius:15px;padding:clamp(10px,1.45vh,17px);display:flex;flex-direction:column;overflow:auto;scrollbar-width:thin;scrollbar-color:#294765 transparent}\n"
    ".panel-head{display:flex;gap:9px;align-items:flex-start;margin-bottom:clamp(7px,1.1vh,12px);flex:0 0 auto}\n"
    ".panel-icon{width:clamp(23px,3vh,29px);height:clamp(23px,3vh,29px)}.panel-icon svg{width:100%;height:100%}\n"
    ".panel h2{font-size:clamp(14px,1.9vh,18px);margin:0 0 2px;font-weight:800}.panel-sub{margin:0;color:var(--muted);font-size:clamp(9px,1.25vh,12px);line-height:1.2}\n"
    ".fields{display:grid;grid-template-columns:1fr 1fr;gap:clamp(6px,.95vh,11px) 9px;align-content:start}.field{min-width:0}.field.full{grid-column:1/-1}\n"
    ".field label{display:block;margin:0 0 4px;color:#c7d6e9;font-size:clamp(9px,1.25vh,12px);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}\n"
    ".hint{color:#7f96b2;font-size:clamp(8px,1.05vh,10px);margin-top:3px;line-height:1.18}\n"
    ".input-wrap{position:relative}.input{width:100%;height:clamp(30px,4.15vh,40px);border-radius:9px;border:1px solid var(--line2);background:#081421;color:#f3f7fd;padding:0 10px;outline:none;font-size:clamp(10px,1.45vh,14px);transition:.18s}.input:focus{border-color:#4d8fe6;box-shadow:0 0 0 3px #438cff1d}.input::placeholder{color:#6f8199}.input.has-eye{padding-right:37px}\n"
    ".eye{position:absolute;right:3px;top:50%;transform:translateY(-50%);width:30px;height:30px;border:0;background:transparent;color:#7895ba;border-radius:8px;display:grid;place-items:center}.eye:hover{background:#ffffff09;color:#a9c8ed}.eye svg{width:16px;height:16px}\n"
    ".switch-row{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-bottom:clamp(7px,1.1vh,11px)}.switch-title{font-size:clamp(10px,1.4vh,13px);font-weight:700}.switch-desc{color:var(--muted);font-size:clamp(8px,1.05vh,10px);margin-top:2px;line-height:1.2}.switch{position:relative;width:46px;height:25px;flex:0 0 46px}.switch input{display:none}.switch span{position:absolute;inset:0;border-radius:99px;background:#314156;border:1px solid #41556f}.switch span:after{content:'';position:absolute;width:19px;height:19px;left:2px;top:2px;background:white;border-radius:50%;transition:.18s;box-shadow:0 3px 8px #0005}.switch input:checked+span{background:#20aa4b;border-color:#30c65d}.switch input:checked+span:after{transform:translateX(22px)}.switch input:disabled+span{opacity:.45}\n"
    ".watch-grid{display:grid;grid-template-columns:1fr 1fr;gap:clamp(6px,.95vh,11px) 9px;align-content:start}\n"
    ".watch-note{margin-top:auto;padding-top:8px;color:#8095b0;font-size:clamp(8px,1.05vh,10px);line-height:1.2}\n"
    "\n"
    ".toast{position:fixed;right:14px;bottom:14px;z-index:80;max-width:420px;padding:11px 14px;border-radius:11px;background:#0d1a2b;border:1px solid var(--line2);box-shadow:var(--shadow);opacity:0;transform:translateY(10px);pointer-events:none;transition:.18s}.toast.show{opacity:1;transform:none}.toast.error{border-color:#7a3140;color:#ffb3bb}.toast.success{border-color:#27683e;color:#b7f7c8}\n"
    ".overlay{position:fixed;inset:0;background:#020814c7;backdrop-filter:blur(5px);display:none;align-items:center;justify-content:center;padding:20px;z-index:100}.overlay.show{display:flex}.modal{width:min(460px,calc(100vw - 24px));max-height:calc(100vh - 24px);overflow:auto;border-radius:20px;background:linear-gradient(180deg,#12233a,#0a1727);border:1px solid #294765;box-shadow:0 30px 90px #000b;padding:22px}.modal-top{display:flex;gap:13px;align-items:flex-start}.modal-symbol{width:42px;height:42px;border-radius:13px;background:#f1495515;color:#ff6872;display:grid;place-items:center;flex:0 0 auto}.modal-symbol.blue{background:#438cff15;color:#65a7ff}.modal-symbol svg{width:22px;height:22px}.modal h3{margin:1px 0 5px;font-size:19px}.modal p{margin:0;color:var(--muted);line-height:1.45;font-size:13px}.modal-actions{display:flex;justify-content:flex-end;gap:10px;margin-top:22px}.modal-btn{height:42px;padding:0 16px;border-radius:10px;border:1px solid #31455d;background:#17263a;color:white;font-weight:700}.modal-btn.danger{border:0;background:#ed3f4c}.countdown{display:none;margin-top:18px}.countdown.show{display:block}.count-num{font-size:48px;font-weight:900;letter-spacing:-.04em;text-align:center}.count-label{text-align:center;color:var(--muted);font-size:12px}.progress{height:8px;margin-top:15px;border-radius:99px;background:#ffffff0f;overflow:hidden}.progress>div{height:100%;width:0;background:#438cff;transition:width .9s linear}\n"
    "@media(max-width:1050px){.hero p{display:none}.health-link{display:none}.btn{padding:0 10px}.panels{grid-template-columns:1.35fr .95fr .72fr 1.28fr}}\n"
    "@media(max-width:760px){.brandmark{display:none}.hero h1{font-size:15px}.badge{display:none!important}.stats{gap:4px}.stat{padding:5px}.stat-icon{display:none}.stat-label{font-size:8px}.stat-value{font-size:9px}.panels{gap:4px}.panel{padding:7px}.panel-icon{display:none}.panel h2{font-size:11px}.panel-sub,.hint,.switch-desc{display:none}.field label{font-size:8px}.input{height:28px;font-size:9px;padding:0 6px}.watch-grid,.fields{gap:5px 4px}.btn{font-size:9px;padding:0 7px}.switch{transform:scale(.82);transform-origin:right center}}\n"
    "</style>\n"
    "</head>\n"
    "<body>\n"
    "<svg width='0' height='0' style='position:absolute'>\n"
    "<symbol id='wifi' viewBox='0 0 24 24'><path d='M4 10a12 12 0 0 1 16 0M7 14a7 7 0 0 1 10 0M10.5 17.5a2.2 2.2 0 0 1 3 0' fill='none' stroke='currentColor' stroke-width='2.1' stroke-linecap='round'/><circle cx='12' cy='20' r='1.2' fill='currentColor'/></symbol>\n"
    "<symbol id='share' viewBox='0 0 24 24'><circle cx='18' cy='5' r='2.5' fill='none' stroke='currentColor' stroke-width='2'/><circle cx='6' cy='12' r='2.5' fill='none' stroke='currentColor' stroke-width='2'/><circle cx='18' cy='19' r='2.5' fill='none' stroke='currentColor' stroke-width='2'/><path d='M8.2 10.8 15.8 6.2M8.2 13.2l7.6 4.6' fill='none' stroke='currentColor' stroke-width='2'/></symbol>\n"
    "<symbol id='server' viewBox='0 0 24 24'><rect x='4' y='4' width='16' height='5' rx='1.5' fill='none' stroke='currentColor' stroke-width='2'/><rect x='4' y='10' width='16' height='5' rx='1.5' fill='none' stroke='currentColor' stroke-width='2'/><rect x='4' y='16' width='16' height='4' rx='1.5' fill='none' stroke='currentColor' stroke-width='2'/><circle cx='7.5' cy='6.5' r='.9' fill='currentColor'/><circle cx='7.5' cy='12.5' r='.9' fill='currentColor'/></symbol>\n"
    "<symbol id='warn' viewBox='0 0 24 24'><path d='M12 4 21 20H3L12 4Z' fill='none' stroke='currentColor' stroke-width='2' stroke-linejoin='round'/><path d='M12 9v5M12 17.4v.1' stroke='currentColor' stroke-width='2' stroke-linecap='round'/></symbol>\n"
    "<symbol id='refresh' viewBox='0 0 24 24'><path d='M20 7v5h-5M4 17v-5h5M18.5 11a7 7 0 0 0-11.8-3.7L4 10M5.5 13a7 7 0 0 0 11.8 3.7L20 14' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'/></symbol>\n"
    "<symbol id='clock' viewBox='0 0 24 24'><circle cx='12' cy='12' r='9' fill='none' stroke='currentColor' stroke-width='2'/><path d='M12 7v5l3 2' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round'/></symbol>\n"
    "<symbol id='route' viewBox='0 0 24 24'><circle cx='6' cy='6' r='2' fill='none' stroke='currentColor' stroke-width='2'/><circle cx='18' cy='6' r='2' fill='none' stroke='currentColor' stroke-width='2'/><circle cx='12' cy='18' r='2' fill='none' stroke='currentColor' stroke-width='2'/><path d='M8 6h8M7.2 7.6l3.7 8.4M16.8 7.6 13.1 16' fill='none' stroke='currentColor' stroke-width='2'/></symbol>\n"
    "<symbol id='pulse' viewBox='0 0 24 24'><path d='M3 13h4l2-7 4 13 3-8 2 4h3' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'/></symbol>\n"
    "<symbol id='eye' viewBox='0 0 24 24'><path d='M2.5 12s3.5-6 9.5-6 9.5 6 9.5 6-3.5 6-9.5 6-9.5-6-9.5-6Z' fill='none' stroke='currentColor' stroke-width='2'/><circle cx='12' cy='12' r='2.5' fill='none' stroke='currentColor' stroke-width='2'/></symbol>\n"
    "</svg>\n"
    "<div class='app'>\n"
    "<header class='hero'>\n"
    "<div class='brand'><div class='brandmark'><svg><use href='#wifi'/></svg></div><div><h1>Modem Watchdog</h1><p>Monitoramento e reinicio automatico do modem</p></div></div>\n"
    "<div class='hero-actions'>\n"
    "<div id='attention' class='badge'><svg><use href='#warn'/></svg>ATENCAO</div>\n"
    "<a class='health-link' href='/health' target='_blank'>Health JSON</a>\n"
    "<button id='save' class='btn btn-primary' type='submit' form='form'>Salvar configuracoes</button>\n"
    "<button id='reboot' class='btn btn-danger' type='button'>Reiniciar modem</button>\n"
    "</div>\n"
    "</header>\n"
    "<div id='pending' class='notice'>Nova rede Wi-Fi em validacao. Se ela nao conectar, a configuracao anterior sera restaurada automaticamente.</div>\n"
    "<section class='stats'>\n"
    "<div class='stat'><div class='stat-icon'><svg><use href='#wifi'/></svg></div><div><div class='stat-label'>Wi-Fi</div><div id='st_wifi' class='stat-value'>-</div></div></div>\n"
    "<div class='stat'><div class='stat-icon'><svg><use href='#server'/></svg></div><div><div class='stat-label'>Local IP</div><div id='st_local' class='stat-value'>-</div></div></div>\n"
    "<div class='stat'><div class='stat-icon'><svg><use href='#share'/></svg></div><div><div class='stat-label'>Tailscale</div><div id='st_ts' class='stat-value'>-</div></div></div>\n"
    "<div class='stat'><div class='stat-icon'><svg><use href='#server'/></svg></div><div><div class='stat-label'>Tailscale IP</div><div id='st_ip' class='stat-value'>-</div></div></div>\n"
    "<div class='stat'><div class='stat-icon'><svg><use href='#warn'/></svg></div><div><div class='stat-label'>Falhas</div><div id='st_fail' class='stat-value'>-</div></div></div>\n"
    "<div class='stat'><div class='stat-icon'><svg><use href='#refresh'/></svg></div><div><div class='stat-label'>Reboots auto</div><div id='st_reboots' class='stat-value'>-</div></div></div>\n"
    "<div class='stat'><div class='stat-icon'><svg><use href='#clock'/></svg></div><div><div class='stat-label'>Estado</div><div id='st_state' class='stat-value'>-</div></div></div>\n"
    "</section>\n"
    "<form id='form'>\n"
    "<div class='panels'>\n"
    "<section class='panel'>\n"
    "<div class='panel-head'><div class='panel-icon'><svg><use href='#wifi'/></svg></div><div><h2>Conectividade</h2><p class='panel-sub'>Configuracoes de rede e autenticacao</p></div></div>\n"
    "<div class='fields'>\n"
    "<div class='field full'><label>Wi-Fi SSID</label><input id='wifi_ssid' class='input' maxlength='32' required></div>\n"
    "<div class='field full'><label>Wi-Fi password</label><div class='input-wrap'><input id='wifi_password' class='input has-eye' type='password' maxlength='64' autocomplete='new-password'><button class='eye' type='button' data-eye='wifi_password'><svg><use href='#eye'/></svg></button></div><div class='hint'>Deixe em branco para manter a senha atual.</div></div>\n"
    "<div class='field'><label>Device name</label><input id='device_name' class='input' maxlength='47'></div>\n"
    "<div class='field'><label>Priority peer IP</label><input id='priority_peer_ip' class='input' maxlength='15' placeholder='optional'></div>\n"
    "<div class='field full'><label>Auth / pre-auth key</label><div class='input-wrap'><input id='auth_key' class='input has-eye' type='password' maxlength='95' autocomplete='new-password'><button class='eye' type='button' data-eye='auth_key'><svg><use href='#eye'/></svg></button></div><div class='hint'>Deixe em branco para manter a chave atual.</div></div>\n"
    "</div>\n"
    "</section>\n"
    "<section class='panel'>\n"
    "<div class='panel-head'><div class='panel-icon'><svg><use href='#server'/></svg></div><div><h2>Control plane</h2><p class='panel-sub'>Conexao com o servidor Tailscale (headscale)</p></div></div>\n"
    "<div class='field' style='margin-bottom:clamp(7px,1.1vh,11px)'><label>Hostname</label><input id='ctrl_host' class='input' maxlength='63' required></div>\n"
    "<div class='switch-row'><div><div class='switch-title'>Use TLS</div><div class='switch-desc'>Porta 443 quando ativo; porta 80 quando desativado.</div></div><label class='switch'><input id='ctrl_tls' type='checkbox'><span></span></label></div>\n"
    "<div class='field'><label>Noise public key</label><input id='noise_pubkey' class='input' maxlength='64' spellcheck='false'><div class='hint'>64 caracteres hexadecimais. Esta chave autentica o control plane.</div></div>\n"
    "</section>\n"
    "<section class='panel'>\n"
    "<div class='panel-head'><div class='panel-icon'><svg><use href='#route'/></svg></div><div><h2>Routing</h2><p class='panel-sub'>Anuncio da rede local</p></div></div>\n"
    "<div class='switch-row'><div><div class='switch-title'>Subnet router</div><div class='switch-desc'>Anuncia a LAN por este ESP.</div></div><label class='switch'><input id='subnet_enabled' type='checkbox'><span></span></label></div>\n"
    "<div class='field'><label>Advertised subnet</label><input id='subnet_route' class='input' maxlength='23' placeholder='192.168.100.0/24'></div>\n"
    "</section>\n"
    "<section class='panel'>\n"
    "<div class='panel-head'><div class='panel-icon'><svg><use href='#pulse'/></svg></div><div><h2>Watchdog</h2><p class='panel-sub'>Monitoramento da internet e reinicio do modem</p></div></div>\n"
    "<div class='watch-grid'>\n"
    "<div class='field'><label>Check interval (s)</label><input id='check_interval_s' class='input' type='number' min='5' max='3600'></div>\n"
    "<div class='field'><label>Failures before reboot</label><input id='failures_before_reboot' class='input' type='number' min='1' max='20'></div>\n"
    "<div class='field'><label>Power-off duration (s)</label><input id='modem_off_s' class='input' type='number' min='1' max='120'></div>\n"
    "<div class='field'><label>Boot grace period (s)</label><input id='modem_boot_s' class='input' type='number' min='10' max='1800'></div>\n"
    "<div class='field'><label>Max auto reboots</label><input id='max_auto_reboots' class='input' type='number' min='1' max='20'></div>\n"
    "<div class='field'><label>Reboot window (s)</label><input id='reboot_window_s' class='input' type='number' min='60' max='86400'></div>\n"
    "</div>\n"
    "<div class='watch-note'>A energia sera cortada por <span id='off_note'>20</span>s. Status atualizado a cada 10s.</div>\n"
    "</section>\n"
    "</div>\n"
    "</form>\n"
    "</div>\n"
    "<div id='toast' class='toast'></div>\n"
    "<div id='overlay' class='overlay'><div class='modal'>\n"
    "<div class='modal-top'><div id='modal_symbol' class='modal-symbol'><svg><use href='#warn'/></svg></div><div><h3 id='modal_title'>Reiniciar modem?</h3><p id='modal_desc'>O ESP vai cortar a energia do modem e ligar novamente.</p></div></div>\n"
    "<div id='countdown' class='countdown'><div class='count-num'><span id='count_value'>20</span>s</div><div id='count_label' class='count-label'>religando a fonte em...</div><div class='progress'><div id='progress'></div></div></div>\n"
    "<div id='modal_actions' class='modal-actions'><button id='cancel' class='modal-btn' type='button'>Cancelar</button><button id='confirm' class='modal-btn danger' type='button'>Reiniciar</button></div>\n"
    "</div></div>\n"
    "<script>\n"
    "const $=id=>document.getElementById(id);\n"
    "let busy=false,settings={},healthTimer=null,countTimer=null,baseline='';\n"
    "function toast(msg,type){const el=$('toast');el.textContent=msg;el.className='toast show '+(type||'success');clearTimeout(el._t);el._t=setTimeout(()=>el.className='toast',3000)}\n"
    "function dot(ok,label){return \"<span class='dot \"+(ok?'ok':'bad')+\"'></span>\"+label}\n"
    "function setv(id,v){$(id).value=v==null?'':v}\n"
    "function modalClose(){if(!busy)$('overlay').classList.remove('show')}\n"
    "function modalOpen(){$('overlay').classList.add('show')}\n"
    "function showConfirm(){\n"
    " busy=false;$('modal_symbol').className='modal-symbol';$('modal_title').textContent='Reiniciar modem?';\n"
    " $('modal_desc').textContent='O ESP vai cortar a energia do modem por '+Number($('modem_off_s').value||20)+' segundos e ligar novamente.';\n"
    " $('countdown').classList.remove('show');$('modal_actions').style.display='flex';$('cancel').style.display='';$('confirm').style.display='';modalOpen()\n"
    "}\n"
    "function showApplying(wifiChanged){\n"
    " busy=true;$('modal_symbol').className='modal-symbol blue';$('modal_title').textContent='Aplicando configuracoes';\n"
    " $('modal_desc').textContent=wifiChanged?'O ESP vai reiniciar e validar a nova rede. Se falhar, a rede anterior sera restaurada.':'O ESP vai reiniciar para aplicar as alteracoes.';\n"
    " $('modal_actions').style.display='none';$('countdown').classList.add('show');$('count_label').textContent='aguardando o ESP voltar...';\n"
    " let left=5;$('count_value').textContent=left;$('progress').style.width='0%';\n"
    " clearInterval(countTimer);countTimer=setInterval(()=>{left--;if(left<0)left=0;$('count_value').textContent=left;$('progress').style.width=((5-left)/5*100)+'%';if(left===0){clearInterval(countTimer);waitForReturn(0)}},1000);modalOpen()\n"
    "}\n"
    "async function waitForReturn(attempt){\n"
    " try{const r=await fetch('/health',{cache:'no-store'});if(r.ok){location.reload();return}}catch(e){}\n"
    " if(attempt>=20){busy=false;$('modal_title').textContent='ESP ainda nao respondeu';$('modal_desc').textContent='A configuracao foi salva. Se o Wi-Fi mudou, o endereco IP tambem pode ter mudado.';$('countdown').classList.remove('show');$('modal_actions').style.display='flex';$('cancel').style.display='none';$('confirm').textContent='Fechar';$('confirm').onclick=()=>location.reload();return}\n"
    " setTimeout(()=>waitForReturn(attempt+1),2000)\n"
    "}\n"
    "async function loadSettings(){\n"
    " const r=await fetch('/api/config',{cache:'no-store'});if(!r.ok)throw new Error('settings '+r.status);const j=await r.json();settings=j;\n"
    " ['wifi_ssid','device_name','ctrl_host','noise_pubkey','subnet_route','priority_peer_ip','check_interval_s','failures_before_reboot','modem_off_s','modem_boot_s','max_auto_reboots','reboot_window_s'].forEach(k=>setv(k,j[k]));\n"
    " $('ctrl_tls').checked=!!j.ctrl_tls;$('subnet_enabled').checked=!!j.subnet_enabled;$('ctrl_tls').disabled=!j.tls_supported;$('subnet_enabled').disabled=!j.subnet_supported;\n"
    " $('wifi_password').placeholder=j.wifi_password_configured?'Configurada - deixe em branco para manter':'Digite a senha';\n"
    " $('auth_key').placeholder=j.auth_key_configured?'Configurada - deixe em branco para manter':'Digite a auth key';\n"
    " $('pending').classList.toggle('show',!!j.wifi_pending);$('off_note').textContent=j.modem_off_s||20;baseline=JSON.stringify(payload());$('save').disabled=true\n"
    "}\n"
    "async function loadHealth(){\n"
    " try{\n"
    "  const r=await fetch('/health',{cache:'no-store'});if(!r.ok)return;const h=await r.json();\n"
    "  $('st_wifi').innerHTML=dot(!!h.wifi,h.wifi?'conectado':'desconectado');\n"
    "  $('st_local').textContent=h.local_ip||'-';$('st_ts').innerHTML=dot(!!h.tailscale_connected,h.tailscale_connected?'conectado':'desconectado');\n"
    "  $('st_ip').textContent=h.tailscale_ip||'-';$('st_fail').textContent=String(h.failures)+' / '+String(h.failures_limit);\n"
    "  $('st_reboots').textContent=String(h.auto_reboots)+' / '+String(h.auto_reboots_limit);$('st_state').textContent=h.state_label||h.state||'-';\n"
    "  $('st_state').classList.toggle('state-warn',h.state!=='monitoring');\n"
    "  const attention=!h.wifi||!h.tailscale_connected||h.failures>0||h.state!=='monitoring';$('attention').classList.toggle('show',attention);\n"
    "  $('pending').classList.toggle('show',!!h.wifi_pending);if(h.modem_off_s)$('off_note').textContent=h.modem_off_s\n"
    " }catch(e){}\n"
    "}\n"
    "function payload(){\n"
    " return {wifi_ssid:$('wifi_ssid').value.trim(),wifi_password:$('wifi_password').value,device_name:$('device_name').value.trim(),auth_key:$('auth_key').value.trim(),\n"
    " ctrl_host:$('ctrl_host').value.trim(),ctrl_tls:$('ctrl_tls').checked,noise_pubkey:$('noise_pubkey').value.trim(),subnet_enabled:$('subnet_enabled').checked,\n"
    " subnet_route:$('subnet_route').value.trim(),priority_peer_ip:$('priority_peer_ip').value.trim(),check_interval_s:Number($('check_interval_s').value),\n"
    " failures_before_reboot:Number($('failures_before_reboot').value),modem_off_s:Number($('modem_off_s').value),modem_boot_s:Number($('modem_boot_s').value),\n"
    " max_auto_reboots:Number($('max_auto_reboots').value),reboot_window_s:Number($('reboot_window_s').value)}\n"
    "}\n"
    "function syncDirty(){if(!baseline||busy)return;$('save').disabled=JSON.stringify(payload())===baseline}\n"
    "async function save(ev){\n"
    " ev.preventDefault();if(busy)return;$('save').disabled=true;\n"
    " try{const r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload())});const j=await r.json();if(!r.ok||!j.ok)throw new Error(j.error||'Falha ao salvar');toast('Configuracoes salvas','success');showApplying(!!j.wifi_changed)}\n"
    " catch(e){toast(e.message||'Falha ao salvar','error');$('save').disabled=false}\n"
    "}\n"
    "async function doReboot(){\n"
    " if(busy)return;busy=true;$('modal_actions').style.display='none';$('countdown').classList.add('show');$('modal_title').textContent='Reiniciando modem';$('modal_desc').textContent='Comando enviado ao ESP.';\n"
    " const total=Math.max(1,Number($('modem_off_s').value||20));let left=total;$('count_value').textContent=left;$('count_label').textContent='religando a fonte em...';$('progress').style.width='0%';\n"
    " try{const r=await fetch('/api/reboot',{method:'POST'});if(!r.ok)throw new Error(await r.text());\n"
    " countTimer=setInterval(()=>{left--;if(left<0)left=0;$('count_value').textContent=left;$('progress').style.width=((total-left)/total*100)+'%';if(left===0){clearInterval(countTimer);location.reload()}},1000)}\n"
    " catch(e){busy=false;$('countdown').classList.remove('show');$('modal_actions').style.display='flex';toast('Falha ao reiniciar modem','error')}\n"
    "}\n"
    "document.querySelectorAll('[data-eye]').forEach(b=>b.onclick=()=>{const i=$(b.getAttribute('data-eye'));i.type=i.type==='password'?'text':'password'});\n"
    "$('form').addEventListener('submit',save);$('form').addEventListener('input',syncDirty);$('form').addEventListener('change',syncDirty);$('reboot').onclick=showConfirm;$('cancel').onclick=modalClose;$('confirm').onclick=doReboot;$('overlay').onclick=e=>{if(e.target===$('overlay'))modalClose()};document.addEventListener('keydown',e=>{if(e.key==='Escape')modalClose()});\n"
    "$('modem_off_s').addEventListener('input',()=>{$('off_note').textContent=$('modem_off_s').value||20});\n"
    "Promise.all([loadSettings(),loadHealth()]).catch(e=>toast('Falha ao carregar dashboard','error'));healthTimer=setInterval(loadHealth,10000);\n"
    "</script>\n"
    "</body>\n"
    "</html>\n";

static esp_err_t dashboard_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, DASHBOARD_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t settings_page_handler(httpd_req_t *req) {
    return dashboard_handler(req);
}

static esp_err_t root_handler(httpd_req_t *req) {
    return dashboard_handler(req);
}

static esp_err_t health_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Connection", "close");
    char vpn_ip[16] = "";
    if (ml) {
        uint32_t ip = microlink_get_vpn_ip(ml);
        if (ip) microlink_ip_to_str(ip, vpn_ip);
    }

    const bool wifi_ok =
        (xEventGroupGetBits(wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
    const bool tailscale_ok = ml && microlink_is_connected(ml);

    char json[768];
    int n = snprintf(
        json, sizeof(json),
        "{\"state\":\"%s\",\"state_label\":\"%s\","
        "\"wifi\":%s,\"tailscale_connected\":%s,"
        "\"local_ip\":\"%s\",\"tailscale_ip\":\"%s\","
        "\"failures\":%d,\"failures_limit\":%u,"
        "\"auto_reboots\":%d,\"auto_reboots_limit\":%u,"
        "\"modem_off_s\":%u,\"modem_boot_s\":%u,"
        "\"check_interval_s\":%u,\"wifi_pending\":%s,"
        "\"heap_free\":%u,\"heap_largest\":%u}",
        watchdog_state_name(wd_state),
        watchdog_state_label(wd_state),
        wifi_ok ? "true" : "false",
        tailscale_ok ? "true" : "false",
        local_ip,
        vpn_ip,
        consecutive_failures,
        (unsigned)app_cfg.failures_before_reboot,
        automatic_reboots,
        (unsigned)app_cfg.max_auto_reboots,
        (unsigned)app_cfg.modem_off_s,
        (unsigned)app_cfg.modem_boot_s,
        (unsigned)app_cfg.check_interval_s,
        app_cfg.wifi_pending ? "true" : "false",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    if (n < 0 || n >= (int)sizeof(json)) {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t reboot_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Connection", "close");
    if (wd_state != WD_NORMAL) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "Modem reboot already in progress.\n");
    }

    manual_reboot_requested = true;
    char msg[96];
    snprintf(msg, sizeof(msg),
             "Reboot requested. Power will be cut for %u seconds.\n",
             (unsigned)app_cfg.modem_off_s);
    return httpd_resp_sendstr(req, msg);
}

static void start_http_server(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 4096;
    config.max_uri_handlers = 12;
    /* The dashboard is a single inline page; it never needs a large pool of
     * simultaneous HTTP sockets. Keeping this small prevents browser
     * keep-alive/poll connections from pinning large lwIP TCP buffers. */
    config.max_open_sockets = 3;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 3;
    config.send_wait_timeout = 3;

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
    const httpd_uri_t settings = {
        .uri = "/settings",
        .method = HTTP_GET,
        .handler = settings_page_handler,
    };
    const httpd_uri_t settings_get = {
        .uri = "/api/settings",
        .method = HTTP_GET,
        .handler = settings_api_get_handler,
    };
    const httpd_uri_t settings_post = {
        .uri = "/api/settings",
        .method = HTTP_POST,
        .handler = settings_api_post_handler,
    };
    /* Compatibility aliases: the unified dashboard uses /api/settings, while
     * /api/config and /api/reboot make the app API self-explanatory for
     * external clients and keep future UI refactors decoupled from the paths. */
    const httpd_uri_t config_get = {
        .uri = "/api/config",
        .method = HTTP_GET,
        .handler = settings_api_get_handler,
    };
    const httpd_uri_t config_post = {
        .uri = "/api/config",
        .method = HTTP_POST,
        .handler = settings_api_post_handler,
    };
    const httpd_uri_t reboot_api = {
        .uri = "/api/reboot",
        .method = HTTP_POST,
        .handler = reboot_handler,
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &health));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &reboot));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &settings));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &settings_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &settings_post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &config_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &config_post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &reboot_api));

    ESP_LOGI(TAG, "Watchdog HTTP UI listening on port 80");
}

/* ============================================================================
 * WiFi
 * ========================================================================== */

static void build_wifi_config(wifi_config_t *cfg,
                              const char *ssid, const char *password) {
    memset(cfg, 0, sizeof(*cfg));

    if (ssid) {
        size_t len = strlen(ssid);
        if (len > sizeof(cfg->sta.ssid)) len = sizeof(cfg->sta.ssid);
        memcpy(cfg->sta.ssid, ssid, len);
    }
    if (password) {
        size_t len = strlen(password);
        if (len > sizeof(cfg->sta.password)) len = sizeof(cfg->sta.password);
        memcpy(cfg->sta.password, password, len);
    }

    cfg->sta.threshold.authmode =
        (password && password[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
}

static void wifi_pending_validation_task(void *arg) {
    (void)arg;
    if (!app_cfg.wifi_pending) {
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGW(TAG, "Validating pending WiFi network '%s' for 30 seconds",
             app_cfg.wifi_ssid);

    EventBits_t bits = xEventGroupWaitBits(
        wifi_event_group, WIFI_CONNECTED_BIT,
        pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));

    if (bits & WIFI_CONNECTED_BIT) {
        app_cfg.wifi_pending = 0;
        app_cfg.prev_wifi_ssid[0] = '\0';
        app_cfg.prev_wifi_pass[0] = '\0';
        esp_err_t err = app_config_save();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Pending WiFi committed: %s", app_cfg.wifi_ssid);
        } else {
            ESP_LOGE(TAG, "Failed to commit validated WiFi: %s",
                     esp_err_to_name(err));
        }
        vTaskDelete(NULL);
        return;
    }

    if (!app_cfg.prev_wifi_ssid[0]) {
        ESP_LOGE(TAG, "Pending WiFi failed and no rollback network is available");
        app_cfg.wifi_pending = 0;
        app_config_save();
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGE(TAG, "Pending WiFi failed; rolling back to '%s'",
             app_cfg.prev_wifi_ssid);

    char rollback_ssid[33];
    char rollback_pass[65];
    copy_str(rollback_ssid, sizeof(rollback_ssid), app_cfg.prev_wifi_ssid);
    copy_str(rollback_pass, sizeof(rollback_pass), app_cfg.prev_wifi_pass);

    copy_str(app_cfg.wifi_ssid, sizeof(app_cfg.wifi_ssid), rollback_ssid);
    copy_str(app_cfg.wifi_pass, sizeof(app_cfg.wifi_pass), rollback_pass);
    app_cfg.prev_wifi_ssid[0] = '\0';
    app_cfg.prev_wifi_pass[0] = '\0';
    app_cfg.wifi_pending = 0;
    app_config_save();

    wifi_config_t wifi_config;
    build_wifi_config(&wifi_config, rollback_ssid, rollback_pass);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_wifi_connect();

    vTaskDelete(NULL);
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data) {
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        local_ip[0] = '\0';
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "WiFi disconnected; reconnecting...");
        esp_wifi_connect();
        return;
    }

    if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        snprintf(local_ip, sizeof(local_ip), IPSTR, IP2STR(&event->ip_info.ip));

        ESP_LOGI(TAG, "WiFi got IP: %s", local_ip);

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

    wifi_config_t wifi_config;
    build_wifi_config(&wifi_config, app_cfg.wifi_ssid, app_cfg.wifi_pass);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "WiFi started; SSID=%s%s",
             app_cfg.wifi_ssid,
             app_cfg.wifi_pending ? " (pending validation)" : "");

    if (app_cfg.wifi_pending) {
        BaseType_t ok = xTaskCreate(
            wifi_pending_validation_task, "wifi_validate", 3072, NULL, 7, NULL);
        if (ok != pdPASS) {
            ESP_LOGE(TAG, "Could not start WiFi validation task");
        }
    }
}

/* ============================================================================
 * MicroLink
 * ========================================================================== */

static int hex_nibble(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static bool decode_noise_key(const char *hex, uint8_t out[32]) {
    if (!hex || strlen(hex) != 64) return false;
    for (size_t i = 0; i < 32; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static uint8_t runtime_noise_pubkey[32];

static void on_microlink_state(microlink_t *handle, microlink_state_t state,
                               void *user_data) {
    ESP_LOGI(TAG, "MicroLink state=%d", (int)state);

    if (state == ML_STATE_CONNECTED) {
        char ip[16];
        microlink_ip_to_str(microlink_get_vpn_ip(handle), ip);
        ESP_LOGI(TAG, "Tailscale connected: %s", ip);
#ifdef CONFIG_ML_ENABLE_SUBNET_ROUTER
        if (app_cfg.subnet_enabled && app_cfg.subnet_route[0] != '\0') {
            ESP_LOGI(TAG, "Advertising subnet route: %s", app_cfg.subnet_route);
        }
#endif
    }
}

static void microlink_task(void *arg) {
    /* The watchdog is already running while we wait. A dead modem at boot
     * therefore cannot strand the appliance before the relay logic starts. */
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);

    const uint8_t *noise_key = NULL;
    if (app_cfg.noise_pubkey_hex[0]) {
        if (decode_noise_key(app_cfg.noise_pubkey_hex, runtime_noise_pubkey)) {
            noise_key = runtime_noise_pubkey;
        } else {
            ESP_LOGE(TAG, "Stored Noise public key is invalid; refusing to start MicroLink");
            vTaskDelete(NULL);
            return;
        }
    }

    uint32_t priority_peer = app_cfg.priority_peer_ip[0]
        ? microlink_parse_ip(app_cfg.priority_peer_ip) : 0;

    microlink_config_t config = {
        .auth_key = app_cfg.auth_key,
        .device_name = app_cfg.device_name[0] ? app_cfg.device_name : NULL,
        .enable_derp = true,
        .enable_stun = true,
        .enable_disco = true,
        .max_peers = CONFIG_ML_MAX_PEERS,
        .wifi_tx_power_dbm = 0,
        .priority_peer_ip = priority_peer,
        .ctrl_host = app_cfg.ctrl_host[0] ? app_cfg.ctrl_host : NULL,
        .ctrl_noise_pubkey = noise_key,
        .ctrl_tls_override = true,
        .ctrl_tls = app_cfg.ctrl_tls != 0,
#ifdef CONFIG_ML_ENABLE_SUBNET_ROUTER
        .advertise_route = app_cfg.subnet_enabled ? app_cfg.subnet_route : "",
        .advertise_route_override = true,
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

    app_config_load();

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
