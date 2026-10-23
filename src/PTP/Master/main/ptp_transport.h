#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Factory default only: 0 = Annex F Ethernet, 1 = Annex D UDP/IPv4 E2E.
 * NVS device settings select the active transport at boot. */
#ifndef PTP_TRANSPORT_UDP_IPV4
#define PTP_TRANSPORT_UDP_IPV4 0
#endif
#if PTP_TRANSPORT_UDP_IPV4 != 0 && PTP_TRANSPORT_UDP_IPV4 != 1
#error "PTP_TRANSPORT_UDP_IPV4 must be 0 or 1"
#endif
#define PTP_TRANSPORT_HEADER_SIZE (PTP_TRANSPORT_UDP_IPV4 ? 28 : 0)

static inline uint16_t ptp_read_be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

static inline void ptp_write_be16(uint8_t *p, uint16_t v)
{
    p[0] = v >> 8;
    p[1] = v;
}

static inline uint16_t ptp_ip_checksum(const uint8_t *p, size_t len)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i += 2) sum += ptp_read_be16(p + i);
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

/* Only unfragmented IPv4 multicast PTP, with consistent IP/UDP lengths,
 * version, message length, and event/general port. Other traffic stays in lwIP.
 * Byte access avoids unaligned reads at Ethernet's 14-byte header boundary. */
static inline bool ptp_ipv4_parse(const uint8_t *frame, size_t len,
                                   size_t *offset, size_t *payload_len)
{
    if (len < 42 || ptp_read_be16(frame + 12) != 0x0800) return false;
    const uint8_t *ip = frame + 14;
    size_t ihl = (ip[0] & 15) * 4;
    size_t total = ptp_read_be16(ip + 2);
    if ((ip[0] >> 4) != 4 || ihl < 20 || total < ihl + 8 + 34 ||
        total > len - 14 || ip[9] != 17 ||
        (ptp_read_be16(ip + 6) & 0xbfff) ||
        memcmp(ip + 16, "\xe0\x00\x01\x81", 4) ||
        ptp_ip_checksum(ip, ihl) != 0) return false;
    const uint8_t *udp = ip + ihl;
    size_t udp_len = ptp_read_be16(udp + 4);
    uint16_t port = ptp_read_be16(udp + 2);
    if (udp_len != total - ihl || udp_len < 8 + 34 ||
        (port != 319 && port != 320)) return false;
    const uint8_t *ptp = udp + 8;
    uint16_t message_len = ptp_read_be16(ptp + 2);
    if ((ptp[1] & 15) != 2 || message_len < 34 || message_len > udp_len - 8 ||
        port != ((ptp[0] & 15) < 8 ? 319 : 320)) return false;
    /* IPv4 permits a zero UDP checksum; validate a supplied checksum. */
    if (ptp_read_be16(udp + 6)) {
        uint32_t sum = 17 + udp_len;
        for (size_t i = 12; i < 20; i += 2) sum += ptp_read_be16(ip + i);
        for (size_t i = 0; i + 1 < udp_len; i += 2) sum += ptp_read_be16(udp + i);
        if (udp_len & 1) sum += (uint16_t)udp[udp_len - 1] << 8;
        while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
        if (sum != 0xffff) return false;
    }
    *offset = 14 + ihl + 8;
    *payload_len = message_len;
    return true;
}

/* Caller supplies source/destination MACs and a zero-initialized frame.
 * source_ip is in network byte order. IPv4 UDP checksum zero is intentional. */
static inline void ptp_ipv4_build(uint8_t *frame, const void *payload,
                                  size_t len, uint32_t source_ip)
{
    ptp_write_be16(frame + 12, 0x0800);
    uint8_t *ip = frame + 14;
    ip[0] = 0x45;
    ptp_write_be16(ip + 2, 28 + len);
    ptp_write_be16(ip + 6, 0x4000); /* Don't fragment. */
    ip[8] = 1; /* Same-subnet multicast by default. */
    ip[9] = 17;
    memcpy(ip + 12, &source_ip, 4);
    memcpy(ip + 16, "\xe0\x00\x01\x81", 4);
    ptp_write_be16(ip + 10, ptp_ip_checksum(ip, 20));
    uint8_t *udp = ip + 20;
    uint16_t port = ((*(const uint8_t *)payload & 15) < 8) ? 319 : 320;
    ptp_write_be16(udp, port);
    ptp_write_be16(udp + 2, port);
    ptp_write_be16(udp + 4, 8 + len);
    memcpy(udp + 8, payload, len);
}
