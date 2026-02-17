/*
 * APNs (Apple Push Notification service) client for ESP-IDF
 *
 * Sends push notifications to iOS devices via Apple's APNs HTTP/2 API.
 * Uses token-based authentication with an APNs .p8 key (ES256).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief APNs client configuration (static, set once at boot)
 */
typedef struct {
    const char *team_id;         /*!< Apple Developer Team ID (10 chars) */
    const char *key_id;          /*!< APNs authentication key ID (10 chars) */
    const char *bundle_id;       /*!< App bundle identifier (apns-topic) */
    const char *apns_key_pem;    /*!< PEM-encoded .p8 key content (null-terminated) */
    bool use_sandbox;            /*!< true = sandbox, false = production */
} apns_config_t;

/**
 * @brief APNs notification payload — all fields are dynamically supplied per push
 */
typedef struct {
    const char *device_token;    /*!< Target device token (64-char hex string) */
    const char *title;           /*!< Alert title */
    const char *body;            /*!< Alert body text */
    int badge;                   /*!< Badge count (-1 to omit) */
    const char *sound;           /*!< Sound name (NULL to omit, "default" for default) */
    const char *custom_payload;  /*!< Extra JSON fields merged at root level (NULL to omit).
                                      Example: "\"type\":\"alert\",\"id\":42" */
} apns_notification_t;

/**
 * @brief Send an Apple Push Notification via APNs HTTP/2 API
 *
 * This function generates a JWT (ES256) from the provided .p8 key,
 * opens an HTTP/2 connection to Apple's APNs server, and sends
 * the notification payload.
 *
 * Prerequisites:
 *   - WiFi must be connected and have internet access
 *   - System time must be synced (for JWT timestamp)
 *
 * @param config       Pointer to APNs configuration (static credentials)
 * @param notification Pointer to notification content (per-push fields)
 *
 * @return
 *   - ESP_OK on success
 *   - ESP_FAIL on connection/send failure
 *   - ESP_ERR_INVALID_ARG if config or notification is NULL
 */
esp_err_t apns_send_notification(const apns_config_t *config,
                                 const apns_notification_t *notification);

#ifdef __cplusplus
}
#endif
