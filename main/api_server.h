/*
 * HTTP API server for APNs push notifications with token management
 *
 * All endpoints require HTTP Basic Authentication.
 * Credentials are set via CONFIG_API_AUTH_USER / CONFIG_API_AUTH_PASS
 * (see Kconfig / sdkconfig.defaults).
 *
 * ── Token registration ────────────────────────────────────────────────
 *
 * POST /token
 *   Register or update a device push token keyed by the device IP address.
 *   The token is written to the send list unless:
 *     • The IP is in the block list  → ignored, reason "blocked"
 *     • The IP+token pair is already identical in the send list → ignored, reason "no_change"
 *   JSON body:
 *     { "ip": "192.168.1.10", "token": "<apns-device-token>" }
 *   Response:
 *     { "status": "ok" | "ignored", "reason": "blocked" | "no_change" }
 *
 * ── Send list CRUD ────────────────────────────────────────────────────
 *
 * GET /tokens/send
 *   List all entries in the send list.
 *   Response: { "count": N, "entries": [{"ip":"...","token":"..."},...] }
 *
 * DELETE /tokens/send
 *   Remove an entry from the send list.
 *   JSON body: { "ip": "..." }
 *   Response: { "status": "ok" }
 *
 * ── Block list CRUD ───────────────────────────────────────────────────
 *
 * GET /tokens/block
 *   List all entries in the block list.
 *   Response: { "count": N, "entries": [{"ip":"...","token":"..."},...] }
 *
 * POST /tokens/block
 *   Add or overwrite an entry in the block list directly.
 *   JSON body: { "ip": "...", "token": "..." }
 *   Response: { "status": "ok" }
 *
 * DELETE /tokens/block
 *   Remove an entry from the block list.
 *   JSON body: { "ip": "..." }
 *   Response: { "status": "ok" }
 *
 * ── Move between lists ────────────────────────────────────────────────
 *
 * POST /tokens/move-to-block
 *   Move an entry from the send list to the block list.
 *   JSON body: { "ip": "..." }
 *   Response: { "status": "ok" }
 *
 * POST /tokens/move-to-send
 *   Move an entry from the block list to the send list.
 *   JSON body: { "ip": "..." }
 *   Response: { "status": "ok" }
 *
 * ── Push notifications ────────────────────────────────────────────────
 *
 * POST /push
 *   Send a push notification to a single explicit device token (fire and forget).
 *   JSON body:
 *     {
 *       "device_token":  "...",
 *       "title":         "...",
 *       "body":          "...",
 *       "badge":         1,
 *       "sound":         "default",     // optional
 *       "custom_payload":"...",         // optional, raw JSON fields
 *       "server_type":   "sandbox"      // optional: "sandbox" (default) | "production"
 *     }
 *
 * POST /blast
 *   Send the same push notification to every token in the send list (fire and forget).
 *   JSON body:
 *     {
 *       "title":         "...",
 *       "body":          "...",
 *       "badge":         1,
 *       "sound":         "default",     // optional
 *       "custom_payload":"...",         // optional, raw JSON fields
 *       "server_type":   "sandbox"      // optional: "sandbox" (default) | "production"
 *     }
 *   Response: { "status": "queued" }
 *   Per-token results are logged to the console.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the HTTP API server (runs in background). */
esp_err_t api_server_start(void);

#ifdef __cplusplus
}
#endif
