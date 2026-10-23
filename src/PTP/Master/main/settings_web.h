#pragma once
#include "cJSON.h"
#include "settings_page.h"

static bool g_settings_rebooting;

static esp_err_t settings_unavailable(httpd_req_t *req, const char *message)
{
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_sendstr(req, message);
}

static esp_err_t settings_error(httpd_req_t *req, const char *message)
{
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, message);
}

static cJSON *settings_body(httpd_req_t *req)
{
    char type[48];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) != ESP_OK ||
        strcmp(type, "application/json") || req->content_len < 2 || req->content_len > 768) return NULL;
    char body[769];
    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) return NULL;
        got += n;
    }
    body[got] = 0;
    return cJSON_ParseWithLengthOpts(body, got + 1, NULL, true);
}

static esp_err_t settings_page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, SETTINGS_PAGE_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t settings_get_handler(httpd_req_t *req)
{
    device_settings_t saved = g_settings;
    if (g_settings_nvs_ready) {
        nvs_handle_t h;
        if (nvs_open("device_cfg", NVS_READONLY, &h) == ESP_OK) {
            device_settings_t candidate = {0};
            size_t size = sizeof(candidate);
            if (nvs_get_blob(h, "settings", &candidate, &size) == ESP_OK &&
                size == sizeof(candidate) && settings_valid(&candidate)) saved = candidate;
            nvs_close(h);
        }
    }
    char body[768];
    snprintf(body, sizeof(body),
        "{\"hostname\":\"%s\",\"ip\":\"%s\",\"netmask\":\"%s\",\"gateway\":\"%s\","
        "\"priority1\":%u,\"priority2\":%u,\"network_mode\":%u,\"udp\":%u,\"domain\":%u,\"sync_log\":%d,\"announce_log\":%d,"
        "\"nvs_ready\":%s,\"reboot_required\":%s,\"survey\":\"%s\"}",
        saved.hostname, saved.ip, saved.netmask, saved.gateway, saved.priority1, saved.priority2, saved.network_mode,
        saved.udp, saved.domain, saved.sync_log, saved.announce_log,
        g_settings_nvs_ready ? "true" : "false",
        memcmp(&saved, &g_settings, sizeof(saved)) ? "true" : "false",
        !initialization_sequence_done ? "Waiting for fix / surveying" :
        g_timing_config_ok ? "Survey complete" : "Configuration failed; see device logs");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static bool settings_json_string(cJSON *root, const char *key, char *out, size_t size)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(v) || strlen(v->valuestring) >= size) return false;
    strcpy(out, v->valuestring);
    return true;
}

static bool settings_json_int(cJSON *root, const char *key, int min, int max, int *out)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < min ||
        v->valuedouble > max || v->valuedouble != (int)v->valuedouble) return false;
    *out = v->valueint;
    return true;
}

static esp_err_t settings_post_handler(httpd_req_t *req)
{
    if (!g_settings_nvs_ready || g_settings_rebooting)
        return settings_unavailable(req, "Settings unavailable");
    cJSON *root = settings_body(req);
    if (!root) return settings_error(req, "Expected a JSON settings object");
    device_settings_t s = {.version = 1};
    int mode, udp, domain, sync, announce, priority1, priority2;
    bool ok = settings_json_string(root, "hostname", s.hostname, sizeof(s.hostname)) &&
        settings_json_string(root, "ip", s.ip, sizeof(s.ip)) &&
        settings_json_string(root, "netmask", s.netmask, sizeof(s.netmask)) &&
        settings_json_string(root, "gateway", s.gateway, sizeof(s.gateway)) &&
        settings_json_int(root, "network_mode", 0, 2, &mode) &&
        settings_json_int(root, "udp", 0, 1, &udp) &&
        settings_json_int(root, "domain", 0, 255, &domain) &&
        settings_json_int(root, "priority1", 0, 255, &priority1) &&
        settings_json_int(root, "priority2", 0, 255, &priority2) &&
        settings_json_int(root, "sync_log", -3, 3, &sync) &&
        settings_json_int(root, "announce_log", -3, 4, &announce);
    cJSON_Delete(root);
    if (!ok) return settings_error(req, "Missing or invalid settings field");
    s.network_mode = mode; s.udp = udp; s.domain = domain;
    s.priority1 = priority1; s.priority2 = priority2;
    s.sync_log = sync; s.announce_log = announce;
    if (!settings_valid(&s)) return settings_error(req,
        "Check hostname, subnet, gateway, and rates. Announce interval must be at least the Sync interval.");
    xSemaphoreTake(g_settings_mutex, portMAX_DELAY);
    nvs_handle_t h;
    esp_err_t err = nvs_open("device_cfg", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, "settings", &s, sizeof(s));
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    xSemaphoreGive(g_settings_mutex);
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS save failed");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"saved\":true,\"reboot_required\":true}");
}

static void settings_reboot_task(void *arg)
{
    /* Let the HTTP response reach the browser before restarting. */
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static esp_err_t settings_action_handler(httpd_req_t *req)
{
    if (g_settings_rebooting) return settings_error(req, "Reboot already scheduled");
    cJSON *root = settings_body(req);
    char action[32];
    bool valid = root && settings_json_string(root, "action", action, sizeof(action));
    cJSON_Delete(root);
    if (!valid || (strcmp(action, "reboot") && strcmp(action, "clear_seed_reboot")))
        return settings_error(req, "Unknown action");
    if (!strcmp(action, "clear_seed_reboot")) {
        if (!g_settings_nvs_ready) return settings_unavailable(req, "NVS unavailable");
        xSemaphoreTake(g_settings_mutex, portMAX_DELAY);
        nvs_handle_t h;
        esp_err_t err = nvs_open("ptp_freq", NVS_READWRITE, &h);
        if (err == ESP_OK) {
            err = nvs_erase_key(h, "learned");
            if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
            if (err == ESP_OK) err = nvs_commit(h);
            nvs_close(h);
        }
        if (err == ESP_OK) atomic_store(&g_seed_clear_pending, true);
        xSemaphoreGive(g_settings_mutex);
        if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Seed erase failed");
    }
    if (xTaskCreate(settings_reboot_task, "cfg_reboot", 2048, NULL, 2, NULL) != pdPASS)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Unable to schedule reboot");
    g_settings_rebooting = true;
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"rebooting\":true}");
}
