/*
 * dns_server.c - Minimal captive DNS server on the softAP (UDP :53).
 *
 * Deliberately tiny: parse the first question, echo it back, append exactly one
 * A record pointing at our softAP address. No recursion, no cache, no upstream,
 * no compression handling beyond refusing pointers in the question. A captive
 * portal needs precisely one answer — "whatever you asked for, it's me" — and
 * every extra feature would be code that can fail on the one path a user has to
 * traverse before the box is usable at all.
 *
 * WHY EVERY QUERY IS ANSWERED, ALWAYS. The reference firmware ran this server in
 * two modes, because its softAP existed to serve appliances and its web UI was
 * blocked on that AP whenever the home network was up: answering everything there
 * would have trapped a passing phone in a portal loop that only ever returned
 * 403s, so outside recovery it answered only the one vendor cloud domain it was
 * hijacking. Neither condition applies here. This box hijacks nothing, and its
 * UI is served on the softAP and the LAN alike, so a client redirected to the
 * portal always lands on something useful. The mode toggle
 * (le_dns_set_captive_all) is therefore intentionally NOT ported — there is no
 * state in which "answer everything with our own address" is the wrong reply.
 *
 * The AP address is read fresh per query rather than cached at start, so the
 * subnet-collision hop in wifi_mgr.c takes effect immediately.
 */
#include "dns_server.h"

#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"

#include "db_config.h"   /* DB_DEFAULT_AP_IP */
#include "wifi_mgr.h"

static const char *TAG = "db_dns";

#define DNS_PORT 53
#define DNS_MAX  512

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} dns_header_t;

/* Decode the QNAME labels at buf+off into a dotted string; return the new offset,
 * or 0 if the name is malformed (or compressed, which is illegal in a question). */
static size_t parse_qname(const uint8_t *buf, size_t len, size_t off,
                          char *out, size_t out_sz)
{
    size_t o = 0;
    size_t wire = 1;                      /* on-wire name bytes; 1 = root label */
    while (off < len && buf[off]) {
        uint8_t l = buf[off++];
        if (l & 0xc0) return 0;           /* no compression in questions */
        /* RFC 1035 caps a name at 255 wire octets. The 128-byte cap below only
         * bounds the dotted-string COPY, not how far `off` walks — and the
         * caller sizes its reply from `off` — so the wire length must be
         * bounded here, not just the copy. */
        wire += 1u + l;
        if (wire > 255) return 0;
        for (int i = 0; i < l && off < len; i++) {
            if (o + 1 < out_sz) out[o++] = buf[off];
            off++;
        }
        if (o + 1 < out_sz) out[o++] = '.';
    }
    if (o && out[o - 1] == '.') o--;      /* strip the trailing dot */
    out[o] = '\0';
    return off + 1;                        /* skip the zero-length root label */
}

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { ESP_LOGE(TAG, "socket failed"); vTaskDelete(NULL); return; }

    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "bind :53 failed — the captive portal will not open "
                      "automatically on client devices");
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "captive DNS listening on :53");

    uint8_t rx[DNS_MAX], tx[DNS_MAX];
    for (;;) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n = recvfrom(sock, rx, sizeof(rx), 0, (struct sockaddr *)&from, &flen);
        if (n < (int)sizeof(dns_header_t)) continue;

        dns_header_t *qh = (dns_header_t *)rx;
        if (ntohs(qh->qdcount) < 1) continue;

        char name[128];
        size_t qend = parse_qname(rx, n, sizeof(dns_header_t), name, sizeof(name));
        /* The question must carry QTYPE + QCLASS, and the reply — the echoed
         * header + question plus the fixed 16-byte answer appended below — must
         * fit tx. parse_qname's 255-byte name cap already guarantees the
         * latter; this check keeps the tx bound even if that cap changes. */
        if (!qend || qend + 4 > (size_t)n) continue;
        if (qend + 4 + 16 > sizeof(tx)) continue;
        uint16_t qtype = (rx[qend] << 8) | rx[qend + 1];
        size_t question_len = (qend + 4) - sizeof(dns_header_t);

        /* Only A (type 1) is answered. Everything else — AAAA in particular — is
         * dropped rather than refused: a phone that gets no AAAA falls back to
         * the A record, which is exactly where we want it. */
        if (qtype != 1) continue;

        /* Resolve our current AP IP fresh each time (survives a subnet hop). */
        char ap_ip[16];
        db_wifi_ap_ip(ap_ip);
        uint32_t ip = ipaddr_addr(ap_ip[0] ? ap_ip : DB_DEFAULT_AP_IP);

        /* Response: copy header + question, set QR and ancount=1, append answer. */
        size_t off = 0;
        memcpy(tx, rx, sizeof(dns_header_t) + question_len);
        dns_header_t *rh = (dns_header_t *)tx;
        rh->flags = htons(0x8180);          /* QR=1, RD copied, RA=1 */
        rh->ancount = htons(1);
        rh->nscount = 0;
        rh->arcount = 0;
        off = sizeof(dns_header_t) + question_len;

        /* Answer: name pointer to the question (0xC00C), type A, class IN, TTL,
         * RDLENGTH 4, then the address. */
        tx[off++] = 0xc0; tx[off++] = 0x0c;
        tx[off++] = 0x00; tx[off++] = 0x01;               /* type A   */
        tx[off++] = 0x00; tx[off++] = 0x01;               /* class IN */
        tx[off++] = 0x00; tx[off++] = 0x00;
        tx[off++] = 0x00; tx[off++] = 0x1e;               /* TTL 30 s: short, so a
                                                             client re-resolves
                                                             quickly once it moves
                                                             onto the real LAN */
        tx[off++] = 0x00; tx[off++] = 0x04;               /* RDLENGTH 4 */
        memcpy(tx + off, &ip, 4); off += 4;               /* A record (net order) */

        sendto(sock, tx, off, 0, (struct sockaddr *)&from, flen);
        ESP_LOGD(TAG, "%s -> %s", name, ap_ip);
    }
}

void db_dns_start(void)
{
    xTaskCreate(dns_task, "db_dns", 4096, NULL, 5, NULL);
}
