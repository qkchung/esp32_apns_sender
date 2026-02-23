# FCM Android Push Notification — Implementation Plan

## Project Context

**Hardware:** ESP32-S3
**Framework:** ESP-IDF v5.5.2
**Target:** `esp32s3`
**Flash:** 16 MB (custom partition table)

This project already implements **iOS push notifications via APNs** (Apple Push Notification
service). The goal is to add **Android push notifications via FCM** (Firebase Cloud Messaging)
as a parallel, independent feature — without touching any existing APNs code.

---

## Existing APNs Implementation (Reference — Do Not Modify)

| File | Role |
|---|---|
| `main/apns.h` | Config + notification structs, public API |
| `main/apns.c` | JWT ES256 generation, HTTP/2 send via sh2lib |
| `main/api_server.h` | `api_server_start()` declaration |
| `main/api_server.c` | HTTP server, `POST /push` endpoint, background task |
| `main/scan.c` | WiFi init, SNTP sync, global `g_apns_config`, `app_main()` |
| `main/certs/apns_auth_key.p8` | EC P-256 private key (PKCS#8), embedded at compile time |
| `main/Kconfig.projbuild` | Menuconfig: WiFi + APNs credentials |
| `sdkconfig.defaults` | Default values for all Kconfig options |
| `main/CMakeLists.txt` | Build config — SRCS, PRIV_REQUIRES, EMBED_TXTFILES |
| `main/idf_component.yml` | Component manager — `sh2lib ^1.1.0`, `nghttp` |

### APNs JWT Signing — ES256 (ECDSA P-256)

```
Key type  : EC P-256 (PKCS#8 PEM, .p8 file)
Algorithm : ES256
mbedtls   : mbedtls_pk_sign() → DER-encoded output
Conversion: der_sig_to_raw() strips ASN.1/DER → raw 64-byte r||s
sig_b64   : base64url(sig_raw[64])  → ~88 chars
JWT header: {"alg":"ES256","kid":"<key_id>"}
JWT payload: {"iss":"<team_id>","iat":<unix_ts>}
Transport : HTTP/2 via sh2lib (nghttp2 wrapper)
Auth usage: JWT used directly as bearer token in every request
```

### Existing Endpoint

```
POST /push
Content-Type: application/json

{
  "device_token": "<64-char iOS device token>",
  "title":        "Alert title",
  "body":         "Alert body",
  "badge":        1,            // optional
  "sound":        "default",    // optional
  "custom_payload": "..."       // optional
}
```

### Task Pattern

`push_handler()` parses JSON → malloc `apns_task_params_t` → `xTaskCreate(apns_send_task, ..., 16384)`
Task calls `apns_send_notification()`, then `free(p)` + `vTaskDelete(NULL)`.

---

## FCM Implementation Plan

### Overview of Differences vs APNs

| Aspect | APNs (existing) | FCM (to add) |
|---|---|---|
| Key type | EC P-256 `.p8` | RSA-2048 PEM (from Firebase service account) |
| JWT algorithm | ES256 | RS256 |
| `der_sig_to_raw()` | Required (DER → 64-byte r\|\|s) | **Not needed** (RSA output is raw bytes) |
| Signature buffer | `sig_der[MBEDTLS_ECDSA_MAX_LEN]` ~72 bytes | `sig_buf[256]` fixed |
| `sig_b64` buffer | `[128]` | `[350]` (ceil(256×4/3)) |
| JWT header | `{"alg":"ES256","kid":"..."}` | `{"alg":"RS256","typ":"JWT"}` |
| JWT payload | `iss` + `iat` only | `iss` + `scope` + `aud` + `iat` + `exp` |
| Token usage | JWT is the bearer token directly | JWT exchanged for OAuth2 access token first |
| Token caching | Not applicable | Access token valid 1 hour — must cache |
| Transport | HTTP/2 via sh2lib | HTTP/1.1 via `esp_http_client` |
| New IDF dependency | sh2lib, nghttp2 | `esp_http_client` (already in ESP-IDF) |

---

### Files to Create (New)

#### 1. `main/fcm.h`

Mirrors `apns.h` structure:

```c
typedef struct {
    const char *project_id;         // Firebase project ID
    const char *sa_email;           // Service account client_email
    const char *sa_private_key_pem; // RSA-2048 PEM, pointer set at boot
} fcm_config_t;

typedef struct {
    const char *device_token;  // Android FCM registration token
    const char *title;
    const char *body;
    const char *data;          // optional: JSON string of extra key-value pairs
} fcm_notification_t;

esp_err_t fcm_send_notification(const fcm_config_t *config,
                                const fcm_notification_t *notification);
```

#### 2. `main/fcm.c`

Internal functions:

```
base64url_encode()
    Identical to apns.c version — copy as-is.

generate_fcm_jwt(config, jwt_buf, jwt_buf_len)
    Same skeleton as generate_jwt() in apns.c, with these changes:
    - header:  {"alg":"RS256","typ":"JWT"}
    - payload: {"iss":"<sa_email>",
                "scope":"https://www.googleapis.com/auth/firebase.messaging",
                "aud":"https://oauth2.googleapis.com/token",
                "iat":<now>,"exp":<now+3600>}
    - payload buffer: [320] (larger than APNs [128])
    - sig buffer:     unsigned char sig_buf[256]  (not sig_der[MBEDTLS_ECDSA_MAX_LEN])
    - sig_len output: 256 bytes for RSA-2048
    - NO der_sig_to_raw() call
    - sig_b64 buffer: [350]
    - signing_input buffer: [700] (longer payload → longer signing input)

fcm_ensure_access_token(config)
    Module-level state:
        static char    s_access_token[2048]
        static time_t  s_token_expiry = 0

    Logic:
        if (time(NULL) < s_token_expiry - 60) return ESP_OK   // still valid

        generate_fcm_jwt(config, jwt, sizeof(jwt))

        POST https://oauth2.googleapis.com/token
            Content-Type: application/x-www-form-urlencoded
            Body: grant_type=urn%3A...%3Ajwt-bearer&assertion=<jwt>

        Parse JSON response:
            "access_token" → copy to s_access_token
            "expires_in"   → s_token_expiry = time(NULL) + expires_in

        Uses esp_http_client with esp_crt_bundle_attach.

fcm_send_notification(config, notification)   [public]
    1. fcm_ensure_access_token(config)
    2. Build FCM v1 JSON:
           {"message":{
               "token":"<device_token>",
               "notification":{"title":"...","body":"..."},
               "data":{...}   // if notification->data != NULL
           }}
    3. POST https://fcm.googleapis.com/v1/projects/<project_id>/messages:send
           Authorization: Bearer <s_access_token>
           Content-Type: application/json
       Uses esp_http_client with esp_crt_bundle_attach.
    4. HTTP 200 → ESP_OK, else log response body + return ESP_FAIL
```

#### 3. `main/certs/fcm_service_account.pem`

RSA private key PEM extracted from Firebase service account JSON.

**How to obtain:**
1. Firebase Console → Project Settings → Service Accounts
2. Click "Generate new private key" → downloads a `.json` file
3. From the JSON, copy the `"private_key"` field value
4. Replace all literal `\n` with real newlines → save as `fcm_service_account.pem`
5. File must start with `-----BEGIN RSA PRIVATE KEY-----` or `-----BEGIN PRIVATE KEY-----`

**Also note from the JSON:**
- `"project_id"` → `CONFIG_FCM_PROJECT_ID` in `sdkconfig.defaults`
- `"client_email"` → `CONFIG_FCM_SA_EMAIL` in `sdkconfig.defaults`

---

### Files to Modify

#### 4. `main/CMakeLists.txt`

```cmake
# Before:
idf_component_register(SRCS "scan.c" "apns.c" "api_server.c"
                    PRIV_REQUIRES esp_wifi nvs_flash esp_netif esp_event
                                  mbedtls esp-tls espressif__nghttp
                                  esp_http_server json
                    INCLUDE_DIRS "."
                    EMBED_TXTFILES "certs/apns_auth_key.p8")

# After: add fcm.c to SRCS, esp_http_client to PRIV_REQUIRES,
#        fcm_service_account.pem to EMBED_TXTFILES
idf_component_register(SRCS "scan.c" "apns.c" "api_server.c" "fcm.c"
                    PRIV_REQUIRES esp_wifi nvs_flash esp_netif esp_event
                                  mbedtls esp-tls espressif__nghttp
                                  esp_http_server esp_http_client json
                    INCLUDE_DIRS "."
                    EMBED_TXTFILES "certs/apns_auth_key.p8"
                                   "certs/fcm_service_account.pem")
```

#### 5. `main/Kconfig.projbuild`

Add a new `menu "Firebase Cloud Messaging Settings"` inside the existing top-level menu:

```kconfig
config FCM_PROJECT_ID
    string "Firebase Project ID"
    default ""
    help
        The Firebase project ID. Found in the Firebase Console
        and in the service account JSON as "project_id".

config FCM_SA_EMAIL
    string "Service Account Email"
    default ""
    help
        The service account client_email from the Firebase
        service account JSON file.
```

#### 6. `sdkconfig.defaults`

Add two lines:

```
CONFIG_FCM_PROJECT_ID="your-firebase-project-id"
CONFIG_FCM_SA_EMAIL="your-sa@your-project.iam.gserviceaccount.com"
```

#### 7. `main/scan.c`

Add alongside the existing APNs symbol and global:

```c
// Embedded FCM RSA key
extern const char fcm_key_start[] asm("_binary_fcm_service_account_pem_start");
extern const char fcm_key_end[]   asm("_binary_fcm_service_account_pem_end");

// Global FCM config (used by api_server.c)
fcm_config_t g_fcm_config;
```

In `app_main()`, after the APNs config block:

```c
g_fcm_config.project_id        = CONFIG_FCM_PROJECT_ID;
g_fcm_config.sa_email          = CONFIG_FCM_SA_EMAIL;
g_fcm_config.sa_private_key_pem = fcm_key_start;
```

#### 8. `main/api_server.c`

Add `#include "fcm.h"` and `extern fcm_config_t g_fcm_config;`

New task params struct:

```c
typedef struct {
    char device_token[256];  // FCM tokens are longer than APNs tokens
    char title[128];
    char body[256];
    char data[256];
    bool has_data;
} fcm_task_params_t;
```

New background task (mirrors `apns_send_task`):

```c
static void fcm_send_task(void *arg)
    // calls fcm_send_notification(&g_fcm_config, &notif)
    // free(p) + vTaskDelete(NULL)
```

New handler:

```c
static esp_err_t fcm_handler(httpd_req_t *req)
    // required JSON: device_token, title, body
    // optional JSON: data
    // xTaskCreate(fcm_send_task, "fcm_send", 20480, ...)
    //   ↑ 20KB — RSA-2048 signing needs more stack than ECDSA
    // respond: "FCM push queued"
```

In `api_server_start()`:
- Register `POST /fcm`
- Bump `config.max_uri_handlers` from `4` to `8`

---

## Token Refresh Flow

```
fcm_send_notification() called
         │
         ▼
fcm_ensure_access_token()
         │
         ├── s_access_token valid (time < expiry - 60s)
         │       └──────────────────────────────────► use cached token
         │
         └── expired or not yet fetched
                  │
                  ▼
            generate_fcm_jwt()
            RS256 sign with RSA-2048 key (~150ms on S3)
                  │
                  ▼
            POST oauth2.googleapis.com/token
            form body: grant_type=...jwt-bearer&assertion=<jwt>
                  │
                  ▼
            parse response JSON
            store s_access_token, s_token_expiry = now + 3600
                  │
                  ▼
            POST fcm.googleapis.com/v1/projects/{id}/messages:send
            Authorization: Bearer <s_access_token>
```

---

## New API Endpoint

```
POST /fcm
Content-Type: application/json

{
  "device_token": "<Android FCM registration token>",
  "title":        "Alert title",
  "body":         "Alert body",
  "data":         "{\"key\":\"value\"}"   // optional
}

Response 200: "FCM push queued"
Response 400: missing required fields
Response 500: task creation failed
```

---

## Memory & Stack Notes

| Operation | Approx. heap / stack |
|---|---|
| RSA-2048 key parsing (mbedtls) | ~8 KB |
| RSA-2048 signing (mbedtls) | ~6 KB |
| HTTPS TLS handshake | ~8 KB |
| FCM task stack | **20 KB** (vs APNs 16 KB) |
| JWT string (RS256) | ~900 bytes on stack |
| OAuth2 access token (cached) | 2 KB static in fcm.c |

---

## Prerequisites Before Coding

- [ ] Firebase service account JSON downloaded
- [ ] `private_key` extracted → `main/certs/fcm_service_account.pem`
- [ ] `project_id` noted → `sdkconfig.defaults`
- [ ] `client_email` noted → `sdkconfig.defaults`
- [ ] Android app already registers with FCM and has a device token to test with
