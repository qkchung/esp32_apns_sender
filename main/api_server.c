/*
 * api_server.c — HTTP API server for APNs push notifications + token management
 *
 * All handlers enforce HTTP Basic Authentication before processing the request.
 */
#include "api_server.h"
#include "apns.h"
#include "token_store.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cJSON.h>
#include <string.h>
#include <stdlib.h>
#include "mbedtls/base64.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "api_server";

extern apns_config_t g_apns_config;

/* ------------------------------------------------------------------ */
/*  Request / task parameter structs                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    char device_token[128];
    char title[128];
    char body[256];
    int  badge;
    char sound[32];
    char custom_payload[256];
    bool has_sound;
    bool has_custom;
    bool use_sandbox;
} apns_task_params_t;

typedef struct {
    char title[128];
    char body[256];
    int  badge;
    char sound[32];
    char custom_payload[256];
    bool has_sound;
    bool has_custom;
    bool use_sandbox;
} blast_params_t;

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/**
 * Verify HTTP Basic Auth header against configured credentials.
 * Sends a 401 response and returns false on failure.
 * Returns true if authenticated.
 */
static bool auth_check(httpd_req_t *req)
{
    /* Get header length first */
    size_t hdr_len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (hdr_len == 0) goto fail;

    char hdr[200];
    if (hdr_len >= sizeof(hdr)) goto fail;
    if (httpd_req_get_hdr_value_str(req, "Authorization",
                                    hdr, sizeof(hdr)) != ESP_OK) goto fail;

    if (strncmp(hdr, "Basic ", 6) != 0) goto fail;

    unsigned char decoded[128] = {0};
    size_t olen = 0;
    if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &olen,
                              (const unsigned char *)(hdr + 6),
                              strlen(hdr + 6)) != 0) goto fail;
    decoded[olen] = '\0';

    char expected[128];
    snprintf(expected, sizeof(expected), "%s:%s",
             CONFIG_API_AUTH_USER, CONFIG_API_AUTH_PASS);

    if (strcmp((char *)decoded, expected) == 0) return true;

fail:
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"API\"");
    httpd_resp_sendstr(req, "{\"error\":\"Unauthorized\"}");
    return false;
}

/** Read request body into buf (null-terminated). Returns bytes read or ≤0 on error. */
static int read_body(httpd_req_t *req, char *buf, size_t len)
{
    int received = httpd_req_recv(req, buf, len - 1);
    if (received > 0) buf[received] = '\0';
    return received;
}

/** Send a JSON string with 200 OK. */
static void send_json_ok(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
}

/** Send a JSON error with the given HTTP status string. */
static void send_json_err(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", msg);
    httpd_resp_sendstr(req, buf);
}

/** Send a paginated token list as chunked JSON. */
static void send_token_list(httpd_req_t *req,
                             token_entry_t *entries, size_t count)
{
    httpd_resp_set_type(req, "application/json");

    char tmp[32];
    httpd_resp_sendstr_chunk(req, "{\"count\":");
    snprintf(tmp, sizeof(tmp), "%zu", count);
    httpd_resp_sendstr_chunk(req, tmp);
    httpd_resp_sendstr_chunk(req, ",\"entries\":[");

    for (size_t i = 0; i < count; i++) {
        if (i > 0) httpd_resp_sendstr_chunk(req, ",");
        /* Worst-case entry: ~165 chars */
        char entry[300];
        snprintf(entry, sizeof(entry),
                 "{\"ip\":\"%s\",\"token\":\"%s\",\"server_type\":\"%s\"}",
                 entries[i].ip, entries[i].token, entries[i].server_type);
        httpd_resp_sendstr_chunk(req, entry);
    }

    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, NULL); /* end chunked response */
}

/** Parse the optional "server_type" field; defaults to sandbox. */
static bool parse_server_type(cJSON *root)
{
    const char *srv = cJSON_GetStringValue(cJSON_GetObjectItem(root, "server_type"));
    return !(srv && strcmp(srv, "production") == 0); /* true = sandbox */
}

/* ------------------------------------------------------------------ */
/*  Background tasks                                                   */
/* ------------------------------------------------------------------ */

static void apns_send_task(void *arg)
{
    apns_task_params_t *p = (apns_task_params_t *)arg;

    apns_config_t cfg = g_apns_config;
    cfg.use_sandbox = p->use_sandbox;

    apns_notification_t notif = {
        .device_token   = p->device_token,
        .title          = p->title,
        .body           = p->body,
        .badge          = p->badge,
        .sound          = p->has_sound  ? p->sound          : NULL,
        .custom_payload = p->has_custom ? p->custom_payload : NULL,
    };

    esp_err_t ret = apns_send_notification(&cfg, &notif);
    ESP_LOGI(TAG, "push [%s] → %s (%s)",
             p->device_token,
             ret == ESP_OK ? "ok" : "fail",
             p->use_sandbox ? "sandbox" : "production");

    free(p);
    vTaskDelete(NULL);
}

static void blast_task(void *arg)
{
    blast_params_t *p = (blast_params_t *)arg;

    apns_config_t cfg = g_apns_config;
    cfg.use_sandbox = p->use_sandbox;

    static token_entry_t entries[TOKEN_MAX_ENTRIES];
    size_t count = 0;
    const char *srv = p->use_sandbox ? "sandbox" : "production";
    token_store_send_list_type(srv, entries, &count, TOKEN_MAX_ENTRIES);

    int ok = 0, fail = 0;
    for (size_t i = 0; i < count; i++) {
        apns_notification_t n = {
            .device_token   = entries[i].token,
            .title          = p->title,
            .body           = p->body,
            .badge          = p->badge,
            .sound          = p->has_sound  ? p->sound          : NULL,
            .custom_payload = p->has_custom ? p->custom_payload : NULL,
        };
        esp_err_t r = apns_send_notification(&cfg, &n);
        if (r == ESP_OK) ok++; else fail++;
        ESP_LOGI(TAG, "blast [%s]: %s", entries[i].ip,
                 r == ESP_OK ? "ok" : "fail");
    }

    ESP_LOGI(TAG, "blast done — %d ok, %d fail (server=%s)",
             ok, fail, p->use_sandbox ? "sandbox" : "production");
    free(p);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/*  Handler: POST /push                                                */
/* ------------------------------------------------------------------ */

static esp_err_t push_handler(httpd_req_t *req)
{
    if (!auth_check(req)) return ESP_OK;

    char buf[600];
    if (read_body(req, buf, sizeof(buf)) <= 0) {
        send_json_err(req, "400 Bad Request", "No body");
        return ESP_OK;
    }

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        send_json_err(req, "400 Bad Request", "Invalid JSON");
        return ESP_OK;
    }

    const char *device_token = cJSON_GetStringValue(cJSON_GetObjectItem(root, "device_token"));
    const char *title        = cJSON_GetStringValue(cJSON_GetObjectItem(root, "title"));
    const char *body         = cJSON_GetStringValue(cJSON_GetObjectItem(root, "body"));

    if (!device_token || !title || !body) {
        cJSON_Delete(root);
        send_json_err(req, "400 Bad Request", "Missing required fields");
        return ESP_OK;
    }

    apns_task_params_t *p = calloc(1, sizeof(apns_task_params_t));
    if (!p) {
        cJSON_Delete(root);
        send_json_err(req, "500 Internal Server Error", "OOM");
        return ESP_OK;
    }

    strncpy(p->device_token, device_token, sizeof(p->device_token) - 1);
    strncpy(p->title,        title,        sizeof(p->title)        - 1);
    strncpy(p->body,         body,         sizeof(p->body)         - 1);

    cJSON *badge_item = cJSON_GetObjectItem(root, "badge");
    p->badge = badge_item ? badge_item->valueint : -1;

    const char *sound = cJSON_GetStringValue(cJSON_GetObjectItem(root, "sound"));
    if (sound) {
        strncpy(p->sound, sound, sizeof(p->sound) - 1);
        p->has_sound = true;
    }

    const char *custom = cJSON_GetStringValue(cJSON_GetObjectItem(root, "custom_payload"));
    if (custom) {
        strncpy(p->custom_payload, custom, sizeof(p->custom_payload) - 1);
        p->has_custom = true;
    }

    p->use_sandbox = parse_server_type(root);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "push queued: token=%.16s... server=%s",
             p->device_token, p->use_sandbox ? "sandbox" : "production");

    if (xTaskCreate(apns_send_task, "apns_push", 16384, p, 5, NULL) != pdPASS) {
        free(p);
        send_json_err(req, "500 Internal Server Error", "Task create failed");
        return ESP_OK;
    }

    send_json_ok(req, "{\"status\":\"queued\"}");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Handler: POST /token                                               */
/* ------------------------------------------------------------------ */

static esp_err_t token_register_handler(httpd_req_t *req)
{
    if (!auth_check(req)) return ESP_OK;

    char buf[350];
    if (read_body(req, buf, sizeof(buf)) <= 0) {
        send_json_err(req, "400 Bad Request", "No body");
        return ESP_OK;
    }

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        send_json_err(req, "400 Bad Request", "Invalid JSON");
        return ESP_OK;
    }

    const char *ip          = cJSON_GetStringValue(cJSON_GetObjectItem(root, "ip"));
    const char *token       = cJSON_GetStringValue(cJSON_GetObjectItem(root, "token"));
    const char *server_type = cJSON_GetStringValue(cJSON_GetObjectItem(root, "server_type"));

    if (!ip || !token) {
        cJSON_Delete(root);
        send_json_err(req, "400 Bad Request", "Missing ip or token");
        return ESP_OK;
    }

    if (!server_type ||
        (strcmp(server_type, "sandbox") != 0 && strcmp(server_type, "production") != 0)) {
        cJSON_Delete(root);
        send_json_err(req, "400 Bad Request", "Missing or invalid server_type (sandbox|production)");
        return ESP_OK;
    }

    /* Guard 1: IP+server_type in block list → ignore */
    char existing[TOKEN_LEN];
    if (token_store_block_get(server_type, ip, existing, sizeof(existing)) == ESP_OK) {
        cJSON_Delete(root);
        send_json_ok(req, "{\"status\":\"ignored\",\"reason\":\"blocked\"}");
        return ESP_OK;
    }

    /* Guard 2: identical server_type+ip+token already in send list → ignore */
    if (token_store_send_get(server_type, ip, existing, sizeof(existing)) == ESP_OK
        && strcmp(existing, token) == 0) {
        cJSON_Delete(root);
        send_json_ok(req, "{\"status\":\"ignored\",\"reason\":\"no_change\"}");
        return ESP_OK;
    }

    esp_err_t ret = token_store_send_set(server_type, ip, token);
    cJSON_Delete(root);

    if (ret != ESP_OK) {
        send_json_err(req, "500 Internal Server Error", "Store write failed");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "token registered: ip=%s server_type=%s", ip, server_type);
    send_json_ok(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Handler: GET /tokens/send                                          */
/* ------------------------------------------------------------------ */

static esp_err_t tokens_send_get_handler(httpd_req_t *req)
{
    if (!auth_check(req)) return ESP_OK;

    static token_entry_t entries[TOKEN_MAX_ENTRIES];
    size_t count = TOKEN_MAX_ENTRIES;
    token_store_send_list(entries, &count, TOKEN_MAX_ENTRIES);
    send_token_list(req, entries, count);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Handler: DELETE /tokens/send                                       */
/* ------------------------------------------------------------------ */

static esp_err_t tokens_send_del_handler(httpd_req_t *req)
{
    if (!auth_check(req)) return ESP_OK;

    char buf[100];
    if (read_body(req, buf, sizeof(buf)) <= 0) {
        send_json_err(req, "400 Bad Request", "No body");
        return ESP_OK;
    }

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        send_json_err(req, "400 Bad Request", "Invalid JSON");
        return ESP_OK;
    }

    const char *ip = cJSON_GetStringValue(cJSON_GetObjectItem(root, "ip"));
    if (!ip) {
        cJSON_Delete(root);
        send_json_err(req, "400 Bad Request", "Missing ip");
        return ESP_OK;
    }

    esp_err_t ret = token_store_send_del(ip);
    cJSON_Delete(root);

    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        send_json_err(req, "404 Not Found", "IP not in send list");
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        send_json_err(req, "500 Internal Server Error", "Store error");
        return ESP_OK;
    }

    send_json_ok(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Handler: GET /tokens/block                                         */
/* ------------------------------------------------------------------ */

static esp_err_t tokens_block_get_handler(httpd_req_t *req)
{
    if (!auth_check(req)) return ESP_OK;

    static token_entry_t entries[TOKEN_MAX_ENTRIES];
    size_t count = TOKEN_MAX_ENTRIES;
    token_store_block_list(entries, &count, TOKEN_MAX_ENTRIES);
    send_token_list(req, entries, count);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Handler: POST /tokens/block                                        */
/* ------------------------------------------------------------------ */

static esp_err_t tokens_block_post_handler(httpd_req_t *req)
{
    if (!auth_check(req)) return ESP_OK;

    char buf[300];
    if (read_body(req, buf, sizeof(buf)) <= 0) {
        send_json_err(req, "400 Bad Request", "No body");
        return ESP_OK;
    }

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        send_json_err(req, "400 Bad Request", "Invalid JSON");
        return ESP_OK;
    }

    const char *ip    = cJSON_GetStringValue(cJSON_GetObjectItem(root, "ip"));
    const char *token = cJSON_GetStringValue(cJSON_GetObjectItem(root, "token"));

    if (!ip || !token) {
        cJSON_Delete(root);
        send_json_err(req, "400 Bad Request", "Missing ip or token");
        return ESP_OK;
    }

    esp_err_t ret = token_store_block_set(ip, token);
    cJSON_Delete(root);

    if (ret != ESP_OK) {
        send_json_err(req, "500 Internal Server Error", "Store write failed");
        return ESP_OK;
    }

    send_json_ok(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Handler: DELETE /tokens/block                                      */
/* ------------------------------------------------------------------ */

static esp_err_t tokens_block_del_handler(httpd_req_t *req)
{
    if (!auth_check(req)) return ESP_OK;

    char buf[100];
    if (read_body(req, buf, sizeof(buf)) <= 0) {
        send_json_err(req, "400 Bad Request", "No body");
        return ESP_OK;
    }

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        send_json_err(req, "400 Bad Request", "Invalid JSON");
        return ESP_OK;
    }

    const char *ip = cJSON_GetStringValue(cJSON_GetObjectItem(root, "ip"));
    if (!ip) {
        cJSON_Delete(root);
        send_json_err(req, "400 Bad Request", "Missing ip");
        return ESP_OK;
    }

    esp_err_t ret = token_store_block_del(ip);
    cJSON_Delete(root);

    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        send_json_err(req, "404 Not Found", "IP not in block list");
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        send_json_err(req, "500 Internal Server Error", "Store error");
        return ESP_OK;
    }

    send_json_ok(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Handler: POST /tokens/move-to-block                               */
/* ------------------------------------------------------------------ */

static esp_err_t move_to_block_handler(httpd_req_t *req)
{
    if (!auth_check(req)) return ESP_OK;

    char buf[100];
    if (read_body(req, buf, sizeof(buf)) <= 0) {
        send_json_err(req, "400 Bad Request", "No body");
        return ESP_OK;
    }

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        send_json_err(req, "400 Bad Request", "Invalid JSON");
        return ESP_OK;
    }

    const char *ip = cJSON_GetStringValue(cJSON_GetObjectItem(root, "ip"));
    if (!ip) {
        cJSON_Delete(root);
        send_json_err(req, "400 Bad Request", "Missing ip");
        return ESP_OK;
    }

    esp_err_t ret = token_store_move_to_block(ip);
    cJSON_Delete(root);

    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        send_json_err(req, "404 Not Found", "IP not in send list");
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        send_json_err(req, "500 Internal Server Error", "Move failed");
        return ESP_OK;
    }

    send_json_ok(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Handler: POST /tokens/move-to-send                                */
/* ------------------------------------------------------------------ */

static esp_err_t move_to_send_handler(httpd_req_t *req)
{
    if (!auth_check(req)) return ESP_OK;

    char buf[100];
    if (read_body(req, buf, sizeof(buf)) <= 0) {
        send_json_err(req, "400 Bad Request", "No body");
        return ESP_OK;
    }

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        send_json_err(req, "400 Bad Request", "Invalid JSON");
        return ESP_OK;
    }

    const char *ip = cJSON_GetStringValue(cJSON_GetObjectItem(root, "ip"));
    if (!ip) {
        cJSON_Delete(root);
        send_json_err(req, "400 Bad Request", "Missing ip");
        return ESP_OK;
    }

    esp_err_t ret = token_store_move_to_send(ip);
    cJSON_Delete(root);

    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        send_json_err(req, "404 Not Found", "IP not in block list");
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        send_json_err(req, "500 Internal Server Error", "Move failed");
        return ESP_OK;
    }

    send_json_ok(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Handler: POST /blast                                               */
/* ------------------------------------------------------------------ */

static esp_err_t blast_handler(httpd_req_t *req)
{
    if (!auth_check(req)) return ESP_OK;

    char buf[600];
    if (read_body(req, buf, sizeof(buf)) <= 0) {
        send_json_err(req, "400 Bad Request", "No body");
        return ESP_OK;
    }

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        send_json_err(req, "400 Bad Request", "Invalid JSON");
        return ESP_OK;
    }

    const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(root, "title"));
    const char *body  = cJSON_GetStringValue(cJSON_GetObjectItem(root, "body"));

    if (!title || !body) {
        cJSON_Delete(root);
        send_json_err(req, "400 Bad Request", "Missing title or body");
        return ESP_OK;
    }

    blast_params_t *p = calloc(1, sizeof(blast_params_t));
    if (!p) {
        cJSON_Delete(root);
        send_json_err(req, "500 Internal Server Error", "OOM");
        return ESP_OK;
    }

    strncpy(p->title, title, sizeof(p->title) - 1);
    strncpy(p->body,  body,  sizeof(p->body)  - 1);

    cJSON *badge_item = cJSON_GetObjectItem(root, "badge");
    p->badge = badge_item ? badge_item->valueint : -1;

    const char *sound = cJSON_GetStringValue(cJSON_GetObjectItem(root, "sound"));
    if (sound) {
        strncpy(p->sound, sound, sizeof(p->sound) - 1);
        p->has_sound = true;
    }

    const char *custom = cJSON_GetStringValue(cJSON_GetObjectItem(root, "custom_payload"));
    if (custom) {
        strncpy(p->custom_payload, custom, sizeof(p->custom_payload) - 1);
        p->has_custom = true;
    }

    p->use_sandbox = parse_server_type(root);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "blast queued (server=%s)",
             p->use_sandbox ? "sandbox" : "production");

    /* 20 KB stack — reads send list + multiple TLS/HTTP2 connections */
    if (xTaskCreate(blast_task, "apns_blast", 20480, p, 5, NULL) != pdPASS) {
        free(p);
        send_json_err(req, "500 Internal Server Error", "Task create failed");
        return ESP_OK;
    }

    send_json_ok(req, "{\"status\":\"queued\"}");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Server start                                                       */
/* ------------------------------------------------------------------ */

esp_err_t api_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 16;

    httpd_handle_t server = NULL;
    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return ret;
    }

#define REG(uri_str, meth, fn) do {                        \
    httpd_uri_t _u = { .uri = (uri_str), .method = (meth), \
                        .handler = (fn), .user_ctx = NULL }; \
    httpd_register_uri_handler(server, &_u);               \
} while (0)

    REG("/push",               HTTP_POST,   push_handler);
    REG("/token",              HTTP_POST,   token_register_handler);
    REG("/tokens/send",        HTTP_GET,    tokens_send_get_handler);
    REG("/tokens/send",        HTTP_DELETE, tokens_send_del_handler);
    REG("/tokens/block",       HTTP_GET,    tokens_block_get_handler);
    REG("/tokens/block",       HTTP_POST,   tokens_block_post_handler);
    REG("/tokens/block",       HTTP_DELETE, tokens_block_del_handler);
    REG("/tokens/move-to-block", HTTP_POST, move_to_block_handler);
    REG("/tokens/move-to-send",  HTTP_POST, move_to_send_handler);
    REG("/blast",              HTTP_POST,   blast_handler);

#undef REG

    ESP_LOGI(TAG, "API server started on port %d", config.server_port);
    return ESP_OK;
}
