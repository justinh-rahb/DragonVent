#include "dv_dns.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include <string.h>

static const char *TAG = "dv_dns";

#define DNS_PORT       53
#define DNS_MAX_LEN    512

static TaskHandle_t s_task = NULL;
static int          s_sock = -1;
static uint32_t     s_redirect_ip = 0;

// DNS header (RFC 1035 §4.1.1)
typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} dns_hdr_t;

// Walk the QNAME (sequence of length-prefixed labels ending in a 0 byte). We
// don't parse the name — we just need the length so we can point past it at
// the QTYPE / QCLASS fields.
static size_t qname_length(const uint8_t *buf, size_t buf_len)
{
    size_t i = 0;
    while (i < buf_len) {
        uint8_t label_len = buf[i];
        if (label_len == 0) return i + 1;
        if (label_len >= 64) return 0;   // compression pointer or malformed
        i += label_len + 1;
    }
    return 0;
}

static void dns_task(void *arg)
{
    uint8_t buf[DNS_MAX_LEN];
    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(DNS_PORT);

    s_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "socket() failed");
        vTaskDelete(NULL);
        return;
    }
    if (bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind() failed");
        close(s_sock); s_sock = -1;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "listening on UDP :%d", DNS_PORT);

    for (;;) {
        struct sockaddr_in src = {0};
        socklen_t src_len = sizeof(src);
        int n = recvfrom(s_sock, buf, sizeof(buf), 0, (struct sockaddr *)&src, &src_len);
        if (n < (int)sizeof(dns_hdr_t)) continue;

        dns_hdr_t *hdr = (dns_hdr_t *)buf;
        uint16_t flags = ntohs(hdr->flags);
        if ((flags & 0x8000) != 0) continue;   // response, not query
        uint16_t qdcount = ntohs(hdr->qdcount);
        if (qdcount == 0) continue;

        size_t off = sizeof(dns_hdr_t);
        size_t qn_len = qname_length(buf + off, n - off);
        if (qn_len == 0 || off + qn_len + 4 > (size_t)n) continue;

        // Only answer A / IN. Reply with the same question + one A record.
        uint16_t qtype  = (buf[off + qn_len] << 8) | buf[off + qn_len + 1];
        uint16_t qclass = (buf[off + qn_len + 2] << 8) | buf[off + qn_len + 3];
        if (qtype != 1 || qclass != 1) continue;

        // Build the response in-place: flip QR + AA, set ancount=1.
        hdr->flags   = htons(0x8180);   // response, RD, RA
        hdr->ancount = htons(1);
        hdr->nscount = 0;
        hdr->arcount = 0;

        size_t resp_len = off + qn_len + 4;
        // Answer: name pointer to offset 12 (the question), TYPE A, CLASS IN,
        //         TTL 60, RDLENGTH 4, RDATA = redirect IP
        uint8_t ans[] = {
            0xC0, 0x0C,                     // name pointer
            0x00, 0x01,                     // TYPE A
            0x00, 0x01,                     // CLASS IN
            0x00, 0x00, 0x00, 0x3C,         // TTL 60
            0x00, 0x04,                     // RDLENGTH
            (uint8_t)(s_redirect_ip),
            (uint8_t)(s_redirect_ip >> 8),
            (uint8_t)(s_redirect_ip >> 16),
            (uint8_t)(s_redirect_ip >> 24),
        };
        if (resp_len + sizeof(ans) > sizeof(buf)) continue;
        memcpy(buf + resp_len, ans, sizeof(ans));
        resp_len += sizeof(ans);

        sendto(s_sock, buf, resp_len, 0, (struct sockaddr *)&src, src_len);
    }
}

esp_err_t dv_dns_start(uint32_t redirect_ip)
{
    if (s_task != NULL) return ESP_ERR_INVALID_STATE;
    s_redirect_ip = redirect_ip;
    if (xTaskCreate(dns_task, "dv_dns", 4096, NULL, 4, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void dv_dns_stop(void)
{
    if (s_sock >= 0) { close(s_sock); s_sock = -1; }
    if (s_task) { vTaskDelete(s_task); s_task = NULL; }
}
