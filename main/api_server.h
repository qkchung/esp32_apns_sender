/*
 * Simple HTTP API server for triggering APNs push notifications
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the HTTP API server (runs in background)
 *
 * POST /push
 *   JSON body: {
 *     "device_token": "...",
 *     "title": "...",
 *     "body": "...",
 *     "badge": 1,
 *     "sound": "default",
 *     "custom_payload": "..." // optional, raw JSON string
 *   }
 *
 * Returns: 200 OK on success, 400/500 on error
 */
esp_err_t api_server_start(void);

#ifdef __cplusplus
}
#endif
