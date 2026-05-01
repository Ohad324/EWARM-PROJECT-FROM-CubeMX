/*
 * pc_discovery.c — UDP broadcast handshake to locate the PC player.
 */
#include "pc_discovery.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "pc_disc";

static char     s_pc_ip[16]   = PC_IP_FALLBACK;
static uint16_t s_pc_port     = PC_DEFAULT_PORT;
static bool     s_discovered  = false;

const char *pc_get_ip(void)   { return s_pc_ip;   }
uint16_t    pc_get_port(void) { return s_pc_port; }

bool pc_discovery_run(uint32_t timeout_ms)
{
    int sock = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { ESP_LOGE(TAG, "socket: errno=%d", errno); return false; }

    int bcast = 1;
    lwip_setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));

    /* Bind to ephemeral local port so we can receive the unicast reply */
    struct sockaddr_in local = { 0 };
    local.sin_family      = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port        = 0;
    if (lwip_bind(sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
        ESP_LOGE(TAG, "bind: errno=%d", errno);
        lwip_close(sock);
        return false;
    }

    struct sockaddr_in dest = { 0 };
    dest.sin_family      = AF_INET;
    dest.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    dest.sin_port        = htons(PC_DISCOVERY_PORT);

    /* Wait timeout_ms total; retry every 500 ms in case PC starts later */
    const uint32_t step = 500;
    uint32_t elapsed = 0;
    char reply[64];

    while (elapsed < timeout_ms) {
        const char *hello = "NORA_HELLO";
        ssize_t n = lwip_sendto(sock, hello, strlen(hello), 0,
                                (struct sockaddr *)&dest, sizeof(dest));
        if (n < 0) ESP_LOGW(TAG, "sendto: errno=%d", errno);
        else        ESP_LOGI(TAG, "broadcast NORA_HELLO → 255.255.255.255:%u",
                             PC_DISCOVERY_PORT);

        struct timeval tv = { .tv_sec = 0, .tv_usec = step * 1000 };
        lwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        struct sockaddr_in from = { 0 };
        socklen_t flen = sizeof(from);
        ssize_t r = lwip_recvfrom(sock, reply, sizeof(reply) - 1, 0,
                                  (struct sockaddr *)&from, &flen);
        if (r > 0) {
            reply[r] = '\0';
            /* Expected: "PLAYER:<ip>:<port>" */
            char ip[16] = {0};
            unsigned port = 0;
            if (sscanf(reply, "PLAYER:%15[^:]:%u", ip, &port) == 2 && port > 0) {
                strncpy(s_pc_ip, ip, sizeof(s_pc_ip) - 1);
                s_pc_ip[sizeof(s_pc_ip) - 1] = '\0';
                s_pc_port    = (uint16_t)port;
                s_discovered = true;
                ESP_LOGI(TAG, "discovered PC: %s:%u (reply='%s')", s_pc_ip, s_pc_port, reply);
                lwip_close(sock);
                return true;
            }
            ESP_LOGW(TAG, "bad reply: '%s'", reply);
        }
        elapsed += step;
    }

    ESP_LOGW(TAG, "discovery timeout — falling back to %s:%u", s_pc_ip, s_pc_port);
    lwip_close(sock);
    return false;
}
