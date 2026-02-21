/*
 * ESP32 APNs (Apple Push Notification) Demo
 *
 * Connects to WiFi, syncs time via SNTP, then sends a push notification
 * to an iOS device through Apple's APNs HTTP/2 API.
 *
 * Configuration:  idf.py menuconfig → "APNs Configuration"
 * APNs key:       Place your .p8 file at main/certs/apns_auth_key.p8
 */

#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "apns.h"
#include "api_server.h"
#include "token_store.h"

static const char *TAG = "main";

/* ---- WiFi connection ---- */

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define WIFI_MAX_RETRY      5
static int s_retry_num = 0;

/* Embedded APNs authentication key (.p8) */
extern const char apns_auth_key_start[] asm("_binary_apns_auth_key_p8_start");
extern const char apns_auth_key_end[]   asm("_binary_apns_auth_key_p8_end");

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "WiFi reconnect attempt %d/%d", s_retry_num, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t h_any, h_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &h_any));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &h_ip));

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid     = CONFIG_APNS_WIFI_SSID,
            .password = CONFIG_APNS_WIFI_PASSWORD,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to WiFi \"%s\" ...", CONFIG_APNS_WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected");
        return ESP_OK;
    }
    ESP_LOGE(TAG, "WiFi connection failed");
    return ESP_FAIL;
}

/* ---- SNTP time synchronisation ---- */

static void sync_time(void)
{
    ESP_LOGI(TAG, "Synchronising time via NTP ...");
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp_cfg);

    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(20000)) != ESP_OK) {
        ESP_LOGW(TAG, "NTP sync failed – JWT timestamps may be wrong");
    } else {
        time_t now;
        struct tm ti;
        time(&now);
        localtime_r(&now, &ti);
        char buf[64];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &ti);
        ESP_LOGI(TAG, "Time synced: %s", buf);
    }
}

/* ---- Global APNs config (used by api_server) ---- */
apns_config_t g_apns_config;

/* ---- Entry point ---- */

void app_main(void)
{
    /* NVS init */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Token store (NVS must be ready first) */
    ESP_ERROR_CHECK(token_store_init());

    /* WiFi */
    if (wifi_init_sta() != ESP_OK) {
        return;
    }

    /* Time sync (JWT needs accurate timestamps) */
    sync_time();

    /* Set up APNs config (global) */
    g_apns_config.team_id      = CONFIG_APNS_TEAM_ID;
    g_apns_config.key_id       = CONFIG_APNS_KEY_ID;
    g_apns_config.bundle_id    = CONFIG_APNS_BUNDLE_ID;
    g_apns_config.apns_key_pem = apns_auth_key_start;
#ifdef CONFIG_APNS_USE_SANDBOX
    g_apns_config.use_sandbox  = true;
#else
    g_apns_config.use_sandbox  = false;
#endif

    /* Init APNs module (creates internal send mutex) */
    ESP_ERROR_CHECK(apns_init());

    /* Start API server */
    api_server_start();

    ESP_LOGI(TAG, "API server ready — HTTP Basic Auth required on all endpoints");
}
