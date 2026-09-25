/*
 * Minimal DNS redirect server derived from the ESP-IDF captive_portal example.
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_netif.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "dns_server.h"

#define DNS_PORT 53
#define DNS_MAX_LEN 256
#define OPCODE_MASK 0x7800
#define QR_FLAG (1 << 7)
#define QD_TYPE_A 0x0001
#define ANS_TTL_SEC 60

static const char *TAG = "rescue_dns";

typedef struct __attribute__((__packed__)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qd_count;
    uint16_t an_count;
    uint16_t ns_count;
    uint16_t ar_count;
} dns_header_t;

typedef struct {
    uint16_t type;
    uint16_t class;
} dns_question_t;

typedef struct __attribute__((__packed__)) {
    uint16_t ptr_offset;
    uint16_t type;
    uint16_t class;
    uint32_t ttl;
    uint16_t addr_len;
    uint32_t ip_addr;
} dns_answer_t;

struct dns_server_handle {
    bool started;
    TaskHandle_t task;
    int num_of_entries;
    dns_entry_pair_t entry[];
};

static char *parse_dns_name(char *raw_name, char *parsed_name,
                            size_t parsed_name_max_len) {
    char *label = raw_name;
    char *name_itr = parsed_name;
    int name_len = 0;

    do {
        int sub_name_len = (uint8_t)*label;
        name_len += sub_name_len + 1;
        if (sub_name_len > 63 || name_len > (int)parsed_name_max_len) {
            return NULL;
        }

        memcpy(name_itr, label + 1, sub_name_len);
        name_itr[sub_name_len] = '.';
        name_itr += sub_name_len + 1;
        label += sub_name_len + 1;
    } while (*label != 0);

    if (name_len <= 0) return NULL;
    parsed_name[name_len - 1] = '\0';
    return label + 1;
}

static int parse_dns_request(char *req, size_t req_len, char *reply,
                             size_t reply_max_len, dns_server_handle_t h) {
    if (req_len < sizeof(dns_header_t) || req_len > reply_max_len) {
        return -1;
    }

    memset(reply, 0, reply_max_len);
    memcpy(reply, req, req_len);

    dns_header_t *header = (dns_header_t *)reply;
    if ((header->flags & OPCODE_MASK) != 0) {
        return 0;
    }

    header->flags |= QR_FLAG;

    uint16_t qd_count = ntohs(header->qd_count);
    if (qd_count == 0 || qd_count > 4) {
        return -1;
    }
    header->an_count = htons(qd_count);

    int reply_len = (int)req_len + qd_count * (int)sizeof(dns_answer_t);
    if (reply_len > (int)reply_max_len) {
        return -1;
    }

    char *cur_ans_ptr = reply + req_len;
    char *cur_qd_ptr = reply + sizeof(dns_header_t);
    char name[128];

    for (int qd_i = 0; qd_i < qd_count; qd_i++) {
        if (cur_qd_ptr >= reply + req_len) return -1;

        char *name_end_ptr = parse_dns_name(cur_qd_ptr, name, sizeof(name));
        if (!name_end_ptr ||
            name_end_ptr + sizeof(dns_question_t) > reply + req_len) {
            return -1;
        }

        dns_question_t *question = (dns_question_t *)name_end_ptr;
        uint16_t qd_type = ntohs(question->type);
        uint16_t qd_class = ntohs(question->class);

        if (qd_type == QD_TYPE_A) {
            esp_ip4_addr_t ip = { .addr = IPADDR_ANY };

            for (int i = 0; i < h->num_of_entries; ++i) {
                if (strcmp(h->entry[i].name, "*") == 0 ||
                    strcmp(h->entry[i].name, name) == 0) {
                    if (h->entry[i].if_key) {
                        esp_netif_t *netif =
                            esp_netif_get_handle_from_ifkey(h->entry[i].if_key);
                        if (netif) {
                            esp_netif_ip_info_t ip_info;
                            if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                                ip.addr = ip_info.ip.addr;
                            }
                        }
                    } else {
                        ip.addr = h->entry[i].ip.addr;
                    }
                    break;
                }
            }

            if (ip.addr != IPADDR_ANY) {
                dns_answer_t *answer = (dns_answer_t *)cur_ans_ptr;
                answer->ptr_offset =
                    htons(0xC000 | (uint16_t)(cur_qd_ptr - reply));
                answer->type = htons(qd_type);
                answer->class = htons(qd_class);
                answer->ttl = htonl(ANS_TTL_SEC);
                answer->addr_len = htons(sizeof(ip.addr));
                answer->ip_addr = ip.addr;
                cur_ans_ptr += sizeof(dns_answer_t);
            }
        }

        cur_qd_ptr = name_end_ptr + sizeof(dns_question_t);
    }

    return (int)(cur_ans_ptr - reply);
}

static void dns_server_task(void *arg) {
    dns_server_handle_t handle = arg;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create DNS socket: errno %d", errno);
        handle->started = false;
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(sock, (struct sockaddr *)&dest, sizeof(dest)) < 0) {
        ESP_LOGE(TAG, "Unable to bind DNS port 53: errno %d", errno);
        close(sock);
        handle->started = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Captive DNS redirect listening on port 53");

    char rx_buffer[128];
    char reply[DNS_MAX_LEN];

    while (handle->started) {
        struct sockaddr_storage source_addr;
        socklen_t source_len = sizeof(source_addr);
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0,
                           (struct sockaddr *)&source_addr, &source_len);
        if (len < 0) {
            if (handle->started) {
                ESP_LOGW(TAG, "DNS recvfrom failed: errno %d", errno);
            }
            break;
        }

        int reply_len =
            parse_dns_request(rx_buffer, (size_t)len, reply, sizeof(reply), handle);
        if (reply_len > 0) {
            if (sendto(sock, reply, reply_len, 0,
                       (struct sockaddr *)&source_addr, source_len) < 0) {
                ESP_LOGW(TAG, "DNS sendto failed: errno %d", errno);
            }
        }
    }

    shutdown(sock, 0);
    close(sock);
    vTaskDelete(NULL);
}

dns_server_handle_t start_dns_server(dns_server_config_t *config) {
    if (!config || config->num_of_entries <= 0 ||
        config->num_of_entries > DNS_SERVER_MAX_ITEMS) {
        return NULL;
    }

    dns_server_handle_t handle =
        calloc(1, sizeof(struct dns_server_handle) +
                     config->num_of_entries * sizeof(dns_entry_pair_t));
    if (!handle) return NULL;

    handle->started = true;
    handle->num_of_entries = config->num_of_entries;
    memcpy(handle->entry, config->item,
           config->num_of_entries * sizeof(dns_entry_pair_t));

    BaseType_t ok =
        xTaskCreate(dns_server_task, "rescue_dns", 4096, handle, 5, &handle->task);
    if (ok != pdPASS) {
        free(handle);
        return NULL;
    }

    return handle;
}

void stop_dns_server(dns_server_handle_t handle) {
    if (!handle) return;

    handle->started = false;
    if (handle->task) {
        vTaskDelete(handle->task);
    }
    free(handle);
}
