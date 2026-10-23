#pragma once

/* Boot-time configuration. Writes do not mutate running timing/network state. */
typedef struct {
    uint32_t version;
    char hostname[32];
    char ip[16], netmask[16], gateway[16];
    uint8_t network_mode; /* 0: DHCP with fallback, 1: static, 2: DHCP only */
    uint8_t udp, domain, priority1, priority2;
    int8_t sync_log, announce_log;
} device_settings_t;

static device_settings_t g_settings;
static bool g_settings_nvs_ready;
static SemaphoreHandle_t g_settings_mutex;
static _Atomic bool g_seed_clear_pending;

static bool settings_ipv4(const char *text, struct in_addr *address)
{
    /* Reject abbreviated, octal, and hexadecimal forms accepted by inet_aton. */
    const char *p = text;
    for (int part = 0; part < 4; part++) {
        unsigned value = 0, digits = 0;
        const char *start = p;
        while (*p >= '0' && *p <= '9') {
            value = value * 10 + (unsigned)(*p++ - '0');
            if (++digits > 3 || value > 255) return false;
        }
        if (!digits || (digits > 1 && *start == '0')) return false;
        if (part < 3) { if (*p++ != '.') return false; }
        else if (*p) return false;
    }
    return inet_aton(text, address) != 0;
}

static bool settings_valid(const device_settings_t *s)
{
    if (s->version != 1 || s->network_mode > 2 || s->udp > 1 ||
        s->sync_log < -3 || s->sync_log > 3 ||
        s->announce_log < s->sync_log || s->announce_log > 4) return false;
    size_t len = strnlen(s->hostname, sizeof(s->hostname));
    if (len >= sizeof(s->hostname)) return false;
    for (size_t i = 0; i < len; i++) {
        char c = s->hostname[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || (c == '-' && i && i + 1 < len))) return false;
    }
    if (!memchr(s->ip, 0, sizeof(s->ip)) ||
        !memchr(s->netmask, 0, sizeof(s->netmask)) ||
        !memchr(s->gateway, 0, sizeof(s->gateway))) return false;
    struct in_addr ip, mask, gw;
    if (!settings_ipv4(s->ip, &ip) || !settings_ipv4(s->netmask, &mask) ||
        !settings_ipv4(s->gateway, &gw)) return false;
    uint32_t host = ntohl(ip.s_addr), m = ntohl(mask.s_addr), g = ntohl(gw.s_addr);
    uint32_t inverse = ~m;
    if (!m || !inverse || (inverse & (inverse + 1)) ||
        (host >> 24) == 0 || (host >> 24) == 127 || (host >> 24) >= 224 ||
        !(host & inverse) || (host & inverse) == inverse) return false;
    if (g && ((g & m) != (host & m) || !(g & inverse) || (g & inverse) == inverse)) return false;
    return true;
}

static void settings_load(void)
{
    g_settings = (device_settings_t){.version = 1, .hostname = DEVICE_HOSTNAME,
        .ip = "192.168.1.27", .netmask = "255.255.255.0", .gateway = "192.168.1.1",
        .udp = PTP_TRANSPORT_UDP_IPV4, .priority1 = 128, .priority2 = 128, .sync_log = 0, .announce_log = 1};
    g_settings_mutex = xSemaphoreCreateMutex();
    g_settings_nvs_ready = g_settings_mutex && nvs_flash_init() == ESP_OK;
    if (!g_settings_nvs_ready) return;
    nvs_handle_t h;
    if (nvs_open("device_cfg", NVS_READONLY, &h) != ESP_OK) return;
    device_settings_t saved = {0};
    size_t size = sizeof(saved);
    esp_err_t err = nvs_get_blob(h, "settings", &saved, &size);
    nvs_close(h);
    if (err == ESP_OK && size == sizeof(saved) && settings_valid(&saved)) g_settings = saved;
}

static unsigned settings_interval_ms(int log)
{
    return log < 0 ? 1000U >> -log : 1000U << log;
}
