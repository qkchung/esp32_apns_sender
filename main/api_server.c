/*
 * Simple HTTP API server for triggering APNs push notifications
 * Uses ESP-IDF's built-in HTTP server
 */
#include "api_server.h"
#include "apns.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cJSON.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "api_server";

extern apns_config_t g_apns_config;

/* Parameters copied out of the HTTP request for the background task */
typedef struct {
    char device_token[128];
    char title[128];
    char body[256];
    int  badge;
    char sound[32];
    char custom_payload[256];
    bool has_sound;
    bool has_custom;
} apns_task_params_t;

static void apns_send_task(void *arg)
{
    apns_task_params_t *p = (apns_task_params_t *)arg;

    apns_notification_t notif = {
        .device_token   = p->device_token,
        .title          = p->title,
        .body           = p->body,
        .badge          = p->badge,
        .sound          = p->has_sound  ? p->sound          : NULL,
        .custom_payload = p->has_custom ? p->custom_payload : NULL,
    };

    esp_err_t ret = apns_send_notification(&g_apns_config, &notif);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "APNs send succeeded");
    } else {
        ESP_LOGE(TAG, "APNs send failed (0x%x)", ret);
    }

    free(p);
    vTaskDelete(NULL);
}

static esp_err_t push_handler(httpd_req_t *req)
{
    char buf[512];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    const char *device_token = cJSON_GetStringValue(cJSON_GetObjectItem(root, "device_token"));
    const char *title        = cJSON_GetStringValue(cJSON_GetObjectItem(root, "title"));
    const char *body         = cJSON_GetStringValue(cJSON_GetObjectItem(root, "body"));

    if (!device_token || !title || !body) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing required fields");
        return ESP_FAIL;
    }

    apns_task_params_t *p = malloc(sizeof(apns_task_params_t));
    if (!p) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    strncpy(p->device_token, device_token, sizeof(p->device_token) - 1);
    strncpy(p->title,        title,        sizeof(p->title)        - 1);
    strncpy(p->body,         body,         sizeof(p->body)         - 1);
    p->device_token[sizeof(p->device_token) - 1] = '\0';
    p->title[sizeof(p->title) - 1]               = '\0';
    p->body[sizeof(p->body)  - 1]                = '\0';

    cJSON *badge_item = cJSON_GetObjectItem(root, "badge");
    p->badge = badge_item ? badge_item->valueint : -1;

    const char *sound = cJSON_GetStringValue(cJSON_GetObjectItem(root, "sound"));
    if (sound) {
        strncpy(p->sound, sound, sizeof(p->sound) - 1);
        p->sound[sizeof(p->sound) - 1] = '\0';
        p->has_sound = true;
    } else {
        p->has_sound = false;
    }

    const char *custom = cJSON_GetStringValue(cJSON_GetObjectItem(root, "custom_payload"));
    if (custom) {
        strncpy(p->custom_payload, custom, sizeof(p->custom_payload) - 1);
        p->custom_payload[sizeof(p->custom_payload) - 1] = '\0';
        p->has_custom = true;
    } else {
        p->has_custom = false;
    }

    cJSON_Delete(root);

    ESP_LOGI(TAG, "Trigger push: token=%s title=%s", p->device_token, p->title);

    /* 16 KB stack — TLS + HTTP/2 + mbedtls need headroom */
    if (xTaskCreate(apns_send_task, "apns_send", 16384, p, 5, NULL) != pdPASS) {
        free(p);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create task");
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "Push queued");
    return ESP_OK;
}

esp_err_t api_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 4;
    httpd_handle_t server = NULL;
    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return ret;
    }
    httpd_uri_t push_uri = {
        .uri      = "/push",
        .method   = HTTP_POST,
        .handler  = push_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &push_uri);
    ESP_LOGI(TAG, "API server started on port %d", config.server_port);
    return ESP_OK;
}
