/* Browser-independent history, held in RAM until reboot. */
typedef struct {
    int64_t bucket;
    int32_t low, high;
    unsigned severity;
    bool valid;
    bool frequency_valid;
    char json[64]; /* Cached only when this ring slot changes. */
} history_sample_t;
/* Each ring has one extra physical slot for fill-before-publish updates. */
static history_sample_t history_samples[3][97];
static _Atomic unsigned history_slots[3][96];
/* 0 = free to read, 1 = reader pinned, UINT_MAX = writer owns spare.
 * A failed claim skips/retries; no task waits, spins, or masks interrupts. */
static _Atomic unsigned history_slot_state[3][97];
static unsigned history_spare[3]; /* single writer only */
static const int history_steps[] = {1, 60, 900};
static const int history_counts[] = {60, 60, 96};
static esp_timer_handle_t history_timer;
static bool history_frequency_started;

static void history_tick(void *arg)
{
    (void)arg;
    int64_t now = esp_timer_get_time() / 1000000;
    led_state_t state = atomic_load(&s_led_state);
    unsigned severity = state == LED_STATE_LOCKED ? 0 :
        state == LED_STATE_ACQUIRING ? 1 : state == LED_STATE_HOLDOVER ? 2 :
        state == LED_STATE_NO_LINK ? 3 : 4;
    int32_t ppb = atomic_load(&g_freq_ppb_hw);
    /* Omit startup placeholders, but retain genuine zero adjustments later. */
    if (ppb != 0) history_frequency_started = true;
    for (int r = 0; r < 3; r++) {
        int64_t bucket = now / history_steps[r];
        unsigned logical = bucket % history_counts[r];
        unsigned spare = history_spare[r];
        unsigned expected = 0;
        if (!atomic_compare_exchange_strong(&history_slot_state[r][spare], &expected, UINT_MAX))
            continue; /* A preempted reader still owns our spare; keep old data. */
        unsigned current = atomic_load(&history_slots[r][logical]);
        history_sample_t *s = &history_samples[r][spare];
        *s = history_samples[r][current];
        if (!s->valid || s->bucket != bucket)
            *s = (history_sample_t){.bucket = bucket, .severity = severity, .valid = true};
        else {
            if (severity > s->severity) s->severity = severity;
        }
        if (history_frequency_started) {
            if (!s->frequency_valid) s->low = s->high = ppb;
            else {
                if (ppb < s->low) s->low = ppb;
                if (ppb > s->high) s->high = ppb;
            }
            s->frequency_valid = true;
        }
        if (s->frequency_valid)
            snprintf(s->json, sizeof(s->json), "[%u,%ld,%ld]", s->severity,
                     (long)s->low, (long)s->high);
        else
            snprintf(s->json, sizeof(s->json), "[%u,null,null]", s->severity);
        /* Publish only after payload and cached JSON are complete. Atomic
         * publication also makes the payload visible on the other core. */
        atomic_store(&history_slot_state[r][spare], 0);
        history_spare[r] = atomic_exchange(&history_slots[r][logical], spare);
    }
}

static esp_err_t history_handler(httpd_req_t *req)
{
    if (!history_timer)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "History unavailable");
    char *buf = malloc(16384);
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "History allocation failed");
    }
    int64_t now = esp_timer_get_time() / 1000000;
    size_t off = web_append(buf, 16384, 0, "{\"ranges\":[");
    for (int r = 0; r < 3; r++) {
        int64_t end = now / history_steps[r];
        off = web_append(buf, 16384, off, "%s{\"step_s\":%d,\"samples\":[", r ? "," : "", history_steps[r]);
        for (int i = 0; i < history_counts[r]; i++) {
            int64_t bucket = end - history_counts[r] + 1 + i;
            char cached[64] = "null";
            for (unsigned attempt = 0; bucket >= 0 && attempt < 3; ++attempt) {
                unsigned slot = atomic_load(&history_slots[r][bucket % history_counts[r]]);
                unsigned expected = 0;
                if (!atomic_compare_exchange_strong(&history_slot_state[r][slot], &expected, 1))
                    continue;
                const history_sample_t *s = &history_samples[r][slot];
                if (s->valid && s->bucket == bucket)
                    memcpy(cached, s->json, sizeof(cached));
                atomic_store(&history_slot_state[r][slot], 0);
                break;
            }
            off = web_append(buf, 16384, off, "%s", i ? "," : "");
            off = web_append(buf, 16384, off, "%s", cached);
        }
        off = web_append(buf, 16384, off, "]}");
    }
    off = web_append(buf, 16384, off, "]}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t result = httpd_resp_send(req, buf, off);
    free(buf);
    return result;
}

static void history_start(void)
{
    if (history_timer) return;
    for (int r = 0; r < 3; ++r) {
        for (int i = 0; i < history_counts[r]; ++i)
            atomic_store(&history_slots[r][i], (unsigned)i);
        history_spare[r] = history_counts[r];
    }
    const esp_timer_create_args_t args = {
        .callback = history_tick, .name = "history", .skip_unhandled_events = true,
    };
    esp_err_t err = esp_timer_create(&args, &history_timer);
    if (err == ESP_OK) {
        history_tick(NULL);
        err = esp_timer_start_periodic(history_timer, 1000000);
    }
    if (err != ESP_OK) ESP_LOGE("WEB", "History timer: %s", esp_err_to_name(err));
}
