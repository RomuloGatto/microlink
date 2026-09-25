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
#define DEFAULT_app_cfg.failures_before_reboot 5U
#define DEFAULT_MODEM_OFF_S            20U
#define DEFAULT_MODEM_BOOT_S           300U
#define DEFAULT_app_cfg.max_auto_reboots       3U
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
    cfg->failures_before_reboot = DEFAULT_app_cfg.failures_before_reboot;
    cfg->modem_off_s = DEFAULT_MODEM_OFF_S;
    cfg->modem_boot_s = DEFAULT_MODEM_BOOT_S;
    cfg->max_auto_reboots = DEFAULT_app_cfg.max_auto_reboots;
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

static esp_err_t root_handler(httpd_req_t *req) {
    char vpn_ip[16] = "not-connected";
    if (ml) {
        uint32_t ip = microlink_get_vpn_ip(ml);
        if (ip) microlink_ip_to_str(ip, vpn_ip);
    }

    const bool wifi_ok =
        (xEventGroupGetBits(wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
    const bool tailscale_ok = ml && microlink_is_connected(ml);
    const bool healthy = wd_state == WD_NORMAL && wifi_ok;

    /* Keep the large, invariant UI strings in flash instead of reserving
     * several KB of scarce internal RAM on classic ESP32. */
    static const char head[] =
        "<!doctype html><html><head>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Modem Watchdog</title>"
        "<style>"
        "*{box-sizing:border-box}body{margin:0;padding:24px;background:#0b0f14;"
        "color:#eef2f7;font:15px -apple-system,BlinkMacSystemFont,Segoe UI,sans-serif}"
        ".c{max-width:560px;margin:auto}.h{display:flex;align-items:center;"
        "justify-content:space-between;margin-bottom:18px}.t{font-size:22px;font-weight:700}"
        ".b{padding:7px 10px;border-radius:999px;color:#fff;font-size:12px;font-weight:700}"
        ".ok{background:#2f9e44}.bad{background:#d9485f}.p{background:#121821;"
        "border:1px solid #202a36;border-radius:18px;padding:18px;box-shadow:0 14px 40px #0006}"
        ".g{display:grid;grid-template-columns:1fr 1fr;gap:10px}.s{background:#0e141c;"
        "border:1px solid #1e2936;border-radius:14px;padding:14px}.l{color:#8190a3;"
        "font-size:12px;margin-bottom:5px}.v{font-size:17px;font-weight:650}"
        ".dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:7px}"
        ".a{display:block;margin-top:14px;color:#9fb0c5;text-decoration:none;font-size:13px}"
        ".btn{width:100%;margin-top:18px;padding:14px;border:0;border-radius:12px;"
        "background:#e5484d;color:#fff;font-size:15px;font-weight:700;cursor:pointer}"
        ".n{text-align:center;color:#68788b;font-size:12px;margin-top:9px}"
        ".ov{position:fixed;inset:0;background:#000a;display:none;align-items:center;"
        "justify-content:center;padding:20px;backdrop-filter:blur(5px);z-index:10}"
        ".ov.show{display:flex}.m{width:min(420px,100%);background:#151c25;border:1px solid #293442;"
        "border-radius:20px;padding:22px;box-shadow:0 24px 80px #000b}.m h3{margin:0 0 8px;font-size:20px}"
        ".m p{margin:0;color:#94a3b5;line-height:1.45}.actions{display:flex;gap:10px;margin-top:22px}"
        ".actions button{flex:1;padding:12px;border:0;border-radius:11px;font-weight:700;cursor:pointer}"
        ".cancel{background:#26313e;color:#e6edf5}.danger{background:#e5484d;color:#fff}"
        ".count{font-size:46px;font-weight:800;text-align:center;margin:20px 0 4px;"
        "font-variant-numeric:tabular-nums}.sub{text-align:center;color:#8190a3;font-size:12px}"
        "</style></head><body><div class='c'>";

    static const char tail[] =
        "</div>"
        "<div id='modal' class='ov'><div class='m'>"
        "<h3 id='mt'>Reiniciar modem?</h3>"
        "<p id='md'>O ESP vai cortar a energia do modem por 20 segundos e ligar novamente.</p>"
        "<div id='count' class='count' style='display:none'></div>"
        "<div id='sub' class='sub' style='display:none'>religando a fonte em...</div>"
        "<div id='actions' class='actions'>"
        "<button class='cancel' onclick='closeModal()'>Cancelar</button>"
        "<button class='danger' onclick='doReboot()'>Reiniciar</button>"
        "</div></div></div>"
        "<script>"
        "const m=document.getElementById('modal'),a=document.getElementById('actions'),"
        "t=document.getElementById('mt'),d=document.getElementById('md'),"
        "c=document.getElementById('count'),s=document.getElementById('sub');"
        "let busy=false;"
        "function openModal(){m.classList.add('show')}"
        "function closeModal(){if(!busy)m.classList.remove('show')}"
        "m.addEventListener('click',e=>{if(e.target===m)closeModal()});"
        "async function doReboot(){"
        "busy=true;a.style.display='none';t.textContent='Reiniciando modem';"
        "d.textContent='Comando enviado ao ESP. A fonte do modem ficara desligada por 20 segundos.';"
        "c.style.display='block';s.style.display='block';"
        "try{"
        "const r=await fetch('/reboot',{method:'POST'});"
        "if(!r.ok)throw new Error(await r.text());"
        "let left=20;c.textContent=left+'s';"
        "const timer=setInterval(()=>{left--;c.textContent=left+'s';"
        "if(left<=0){clearInterval(timer);location.reload()}},1000);"
        "}catch(e){busy=false;c.style.display='none';s.style.display='none';"
        "t.textContent='Nao foi possivel reiniciar';d.textContent=e.message||'Erro ao enviar comando.';"
        "a.innerHTML=\"<button class='cancel' onclick='location.reload()'>Fechar</button>\";"
        "a.style.display='flex';}}"
        "setInterval(()=>{if(!busy&&!m.classList.contains('show'))location.reload()},10000);"
        "</script></body></html>";

    static char status[1400];
    int n = snprintf(
        status, sizeof(status),
        "<div class='h'><div class='t'>Modem Watchdog</div><div class='b %s'>%s</div></div>"
        "<div class='p'><div class='g'>"
        "<div class='s'><div class='l'>Wi-Fi</div><div class='v'><span class='dot %s'></span>%s</div></div>"
        "<div class='s'><div class='l'>Tailscale</div><div class='v'>%s</div></div>"
        "<div class='s'><div class='l'>Tailscale IP</div><div class='v'>%s</div></div>"
        "<div class='s'><div class='l'>Falhas</div><div class='v'>%d / %d</div></div>"
        "<div class='s'><div class='l'>Reboots auto</div><div class='v'>%d / %d</div></div>"
        "<div class='s'><div class='l'>Estado</div><div class='v'>%s</div></div>"
        "</div><a class='a' href='/health'>Ver JSON de health &#8594;</a>"
        "<button class='btn' type='button' onclick='openModal()'>Reiniciar modem</button>"
        "<div class='n'>A energia sera cortada por 20 segundos · status atualiza a cada 10s</div>"
        "</div>",
        healthy ? "ok" : "bad",
        healthy ? "ONLINE" : "ATENCAO",
        wifi_ok ? "ok" : "bad",
        wifi_ok ? "conectado" : "desconectado",
        tailscale_ok ? "conectado" : "desconectado",
        vpn_ip,
        consecutive_failures, app_cfg.failures_before_reboot,
        automatic_reboots, app_cfg.max_auto_reboots,
        watchdog_state_name(wd_state));

    if (n < 0 || n >= (int)sizeof(status)) {
        ESP_LOGE(TAG, "Watchdog UI status overflow (%d bytes)", n);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html");
    if (httpd_resp_send_chunk(req, head, HTTPD_RESP_USE_STRLEN) != ESP_OK ||
        httpd_resp_send_chunk(req, status, HTTPD_RESP_USE_STRLEN) != ESP_OK ||
        httpd_resp_send_chunk(req, tail, HTTPD_RESP_USE_STRLEN) != ESP_OK) {
        return ESP_FAIL;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
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
