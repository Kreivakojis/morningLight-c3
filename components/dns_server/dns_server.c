#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "dns_server.h"

static const char *TAG = "dns_server";

#define DNS_PORT 53
#define DNS_MAX_LEN 512

// DNS header structure
typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qd_count;  // Question count
    uint16_t an_count;  // Answer count
    uint16_t ns_count;  // Authority count
    uint16_t ar_count;  // Additional count
} dns_header_t;

// DNS flags
#define DNS_QR_RESPONSE     (1 << 15)
#define DNS_OPCODE_QUERY    (0 << 11)
#define DNS_AA              (1 << 10)  // Authoritative Answer
#define DNS_TC              (0 << 9)   // Not truncated
#define DNS_RD              (1 << 8)   // Recursion Desired
#define DNS_RA              (0 << 7)   // Recursion not Available
#define DNS_RCODE_OK        0

// DNS record types
#define DNS_TYPE_A          1
#define DNS_CLASS_IN        1

// State
static struct {
    bool running;
    int sock;
    TaskHandle_t task;
    uint32_t ap_ip;
} state = {0};

static int parse_dns_name(const uint8_t *buf, int buf_len, int offset, char *name, int name_len)
{
    int i = offset;
    int j = 0;
    int jumped = 0;
    int jump_offset = 0;

    while (i < buf_len && buf[i] != 0) {
        if ((buf[i] & 0xC0) == 0xC0) {
            // Compression pointer
            if (i + 1 >= buf_len) break;
            if (!jumped) jump_offset = i + 2;
            i = ((buf[i] & 0x3F) << 8) | buf[i + 1];
            jumped = 1;
            continue;
        }

        int label_len = buf[i++];
        if (i + label_len > buf_len) break;

        if (j > 0 && j < name_len - 1) name[j++] = '.';

        for (int k = 0; k < label_len && j < name_len - 1; k++) {
            name[j++] = buf[i++];
        }
    }

    name[j] = '\0';

    if (jumped) return jump_offset;
    return i + 1;  // Skip null terminator
}

static int build_dns_response(const uint8_t *query, int query_len, uint8_t *response, uint32_t ip)
{
    if (query_len < sizeof(dns_header_t)) return -1;

    dns_header_t *q_header = (dns_header_t *)query;
    dns_header_t *r_header = (dns_header_t *)response;

    // Copy query
    memcpy(response, query, query_len);

    // Modify header for response
    r_header->flags = htons(DNS_QR_RESPONSE | DNS_AA | DNS_RCODE_OK);
    r_header->an_count = htons(1);  // One answer

    // Find end of question section
    int offset = sizeof(dns_header_t);
    uint16_t qd_count = ntohs(q_header->qd_count);

    for (int i = 0; i < qd_count && offset < query_len; i++) {
        // Skip name
        while (offset < query_len && query[offset] != 0) {
            if ((query[offset] & 0xC0) == 0xC0) {
                offset += 2;
                break;
            }
            offset += query[offset] + 1;
        }
        if (offset < query_len && query[offset] == 0) offset++;
        offset += 4;  // Skip QTYPE and QCLASS
    }

    // Add answer
    int ans_offset = offset;

    // Name pointer to question
    response[ans_offset++] = 0xC0;
    response[ans_offset++] = sizeof(dns_header_t);

    // Type A
    response[ans_offset++] = 0;
    response[ans_offset++] = DNS_TYPE_A;

    // Class IN
    response[ans_offset++] = 0;
    response[ans_offset++] = DNS_CLASS_IN;

    // TTL (300 seconds)
    response[ans_offset++] = 0;
    response[ans_offset++] = 0;
    response[ans_offset++] = 0x01;
    response[ans_offset++] = 0x2C;

    // Data length (4 bytes for IPv4)
    response[ans_offset++] = 0;
    response[ans_offset++] = 4;

    // IP address (already in network byte order)
    memcpy(&response[ans_offset], &ip, 4);
    ans_offset += 4;

    return ans_offset;
}

static void dns_server_task(void *arg)
{
    uint8_t rx_buffer[DNS_MAX_LEN];
    uint8_t tx_buffer[DNS_MAX_LEN];
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    ESP_LOGI(TAG, "DNS server task started");

    while (state.running) {
        int len = recvfrom(state.sock, rx_buffer, sizeof(rx_buffer), 0,
                          (struct sockaddr *)&client_addr, &addr_len);

        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            ESP_LOGE(TAG, "recvfrom failed: %d", errno);
            break;
        }

        if (len < sizeof(dns_header_t)) {
            continue;
        }

        // Log query for debugging
        char name[64] = {0};
        parse_dns_name(rx_buffer, len, sizeof(dns_header_t), name, sizeof(name));
        ESP_LOGD(TAG, "DNS query for: %s", name);

        // Build and send response
        int resp_len = build_dns_response(rx_buffer, len, tx_buffer, state.ap_ip);
        if (resp_len > 0) {
            sendto(state.sock, tx_buffer, resp_len, 0,
                   (struct sockaddr *)&client_addr, addr_len);
        }
    }

    ESP_LOGI(TAG, "DNS server task stopped");
    vTaskDelete(NULL);
}

esp_err_t dns_server_start(void)
{
    if (state.running) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Starting DNS server");

    // Get AP IP address
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (netif == NULL) {
        ESP_LOGE(TAG, "AP interface not found");
        return ESP_ERR_NOT_FOUND;
    }

    esp_netif_ip_info_t ip_info;
    esp_err_t ret = esp_netif_get_ip_info(netif, &ip_info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get AP IP: %s", esp_err_to_name(ret));
        return ret;
    }

    state.ap_ip = ip_info.ip.addr;
    ESP_LOGI(TAG, "AP IP: " IPSTR, IP2STR(&ip_info.ip));

    // Create UDP socket
    state.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (state.sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket: %d", errno);
        return ESP_FAIL;
    }

    // Set socket timeout
    struct timeval tv = {
        .tv_sec = 1,
        .tv_usec = 0,
    };
    setsockopt(state.sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Bind to port 53
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(state.sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind socket: %d", errno);
        close(state.sock);
        return ESP_FAIL;
    }

    state.running = true;

    // Create task
    BaseType_t xRet = xTaskCreate(dns_server_task, "dns_server", 4096, NULL, 5, &state.task);
    if (xRet != pdPASS) {
        ESP_LOGE(TAG, "Failed to create task");
        close(state.sock);
        state.running = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "DNS server started");
    return ESP_OK;
}

esp_err_t dns_server_stop(void)
{
    if (!state.running) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping DNS server");

    state.running = false;

    // Close socket to unblock recvfrom
    if (state.sock >= 0) {
        close(state.sock);
        state.sock = -1;
    }

    // Wait for task to exit
    vTaskDelay(pdMS_TO_TICKS(100));

    return ESP_OK;
}

bool dns_server_is_running(void)
{
    return state.running;
}
