# Plan: Push Token Management & Blast Notification API

## Context Summary

- **Platform**: ESP32 (ESP-IDF), 16 MB flash
- **Existing stack**: HTTP server (`esp_http_server`), APNs HTTP/2 client (`apns.c`), NVS for persistent storage
- **Current API**: single `POST /push` endpoint — no auth, no token management

---

## Goals

1. Persist device push tokens (IP → token) in flash (NVS)
2. Two managed lists: **Send List** and **Block List**
3. CRUD APIs for both lists + move operations between them
4. `POST /token` — register/update a device token by its IP, auto-adds to send list; write is skipped (silently ignored) if the IP is in the block list, or if the same IP+token pair already exists in the send list
5. `POST /blast` — fire same push notification to every token in send list
6. **HTTP Basic Auth** on every endpoint (credentials in `sdkconfig.defaults`)
7. `/push` and `/blast` accept optional `"server_type"` field: `"sandbox"` (default) or `"production"` — overrides the compile-time `CONFIG_APNS_USE_SANDBOX` setting per request

---

## Architecture Decisions

### Storage: NVS (ESP-IDF built-in KV flash store)
- Partition already present: `nvs, data, nvs, 0x9000, 0x6000` (24 KB — fits ~200+ entries)
- NVS key limit = 15 chars → IPv4 max = 15 chars (`255.255.255.255`) — fits exactly
- Two NVS namespaces:
  - `"tok_send"` — send list
  - `"tok_blk"` — block list
- Key = IP string, Value = device token string (≤100 chars)
- Enumerate via `nvs_entry_find` / `nvs_entry_next` / `nvs_entry_info`

### Basic Auth
- Add `CONFIG_API_AUTH_USER` and `CONFIG_API_AUTH_PASS` to `Kconfig.projbuild` + `sdkconfig.defaults`
- Decode `Authorization: Basic <b64>` header using `mbedtls_base64_decode` (already linked)
- Common helper `auth_check(req)` called at top of every handler

### APNs Blast
- `apns_send_notification` uses static globals — **not re-entrant**
- Blast task iterates the send list **sequentially**, calling `apns_send_notification` once per token
- Each call opens its own HTTP/2 connection (current design); acceptable for small fleets

### server_type Parameter (`/push` and `/blast`)
- Optional JSON field `"server_type"`: `"sandbox"` or `"production"`
- Defaults to `"sandbox"` if omitted or unrecognised
- Implemented by copying `g_apns_config` into a local `apns_config_t` and overriding `.use_sandbox`
  before passing to `apns_send_notification` — no global state mutation
- Both `apns_task_params_t` and `blast_params_t` gain a `bool use_sandbox` field

### URI Design (ESP-IDF compatible, no path params)
| Method | URI | Action |
|--------|-----|--------|
| POST | `/token` | Register/update token → add to send list |
| GET | `/tokens/send` | List send list (JSON array) |
| DELETE | `/tokens/send` | Remove from send list (body: `{"ip":"..."}`) |
| GET | `/tokens/block` | List block list |
| POST | `/tokens/block` | Add to block list (body: `{"ip":"...","token":"..."}`) |
| DELETE | `/tokens/block` | Remove from block list (body: `{"ip":"..."}`) |
| POST | `/tokens/move-to-block` | Move IP from send → block |
| POST | `/tokens/move-to-send` | Move IP from block → send |
| POST | `/blast` | Send push to all send-list tokens (`"server_type"` optional) |
| POST | `/push` | Send push to explicit token (`"server_type"` optional) |

All methods return `Content-Type: application/json`.

---

## Files to Create / Modify

### New: `main/token_store.h`
```
Public API:
  esp_err_t token_store_init(void);

  // Send list
  esp_err_t token_store_send_set(const char *ip, const char *token);
  esp_err_t token_store_send_get(const char *ip, char *tok_out, size_t len);
  esp_err_t token_store_send_del(const char *ip);
  esp_err_t token_store_send_list(token_entry_t *out, size_t *count, size_t max);

  // Block list
  esp_err_t token_store_block_set(const char *ip, const char *token);
  esp_err_t token_store_block_get(const char *ip, char *tok_out, size_t len);
  esp_err_t token_store_block_del(const char *ip);
  esp_err_t token_store_block_list(token_entry_t *out, size_t *count, size_t max);

  // Move
  esp_err_t token_store_move_to_block(const char *ip);  // send→block
  esp_err_t token_store_move_to_send(const char *ip);   // block→send

  typedef struct { char ip[16]; char token[100]; } token_entry_t;
  #define TOKEN_MAX_ENTRIES 64
```

### New: `main/token_store.c`
- Open NVS namespaces at init
- `set`: `nvs_set_str` → `nvs_commit`
- `get`: `nvs_get_str`
- `del`: `nvs_erase_key` → `nvs_commit`
- `list`: `nvs_entry_find` loop to collect all string entries in namespace
- `move_to_block`: `send_get` → `block_set` → `send_del`
- `move_to_send`: `block_get` → `send_set` → `block_del`

### Modified: `main/api_server.h`
- Updated doc comment with all new endpoints
- No new public function signatures needed (still just `api_server_start`)

### Modified: `main/api_server.c`
- Add `#include "token_store.h"` and `mbedtls/base64.h`
- Add `auth_check(httpd_req_t*)` helper (reads Authorization header, base64-decodes, compares to config)
- Extend `apns_task_params_t` with `bool use_sandbox` field (parsed from `"server_type"` in `/push` body)
- Add `blast_params_t` struct (same notification fields as `apns_task_params_t` + `bool use_sandbox`)
- Add handlers: `token_register_handler`, `tokens_send_get_handler`, `tokens_send_del_handler`,
  `tokens_block_get_handler`, `tokens_block_post_handler`, `tokens_block_del_handler`,
  `move_to_block_handler`, `move_to_send_handler`, `blast_handler`
- `push_handler`: parse `"server_type"` field → set `p->use_sandbox`; pass local config copy to `apns_send_notification`
- Blast task: loads send list, builds local `apns_config_t` copy with `use_sandbox` from params, loops `apns_send_notification` per entry
- Update `api_server_start`: raise `max_uri_handlers` to 16, register all URIs

### Modified: `main/Kconfig.projbuild`
Add under new `menu "API Authentication"`:
```
config API_AUTH_USER
    string "HTTP API username"
    default "admin"

config API_AUTH_PASS
    string "HTTP API password"
    default "changeme"
```

### Modified: `sdkconfig.defaults`
```
CONFIG_API_AUTH_USER="admin"
CONFIG_API_AUTH_PASS="changeme"
```

### Modified: `main/CMakeLists.txt`
Add `"token_store.c"` to `SRCS`, add `nvs_flash` to `PRIV_REQUIRES` (already present — verify).

### Modified: `main/scan.c` (minor)
Call `token_store_init()` after NVS init, before `api_server_start()`.

---

## Key Implementation Details

### auth_check helper
```c
static bool auth_check(httpd_req_t *req) {
    char hdr[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK)
        goto fail;
    if (strncmp(hdr, "Basic ", 6) != 0) goto fail;

    unsigned char decoded[64] = {0};
    size_t olen = 0;
    mbedtls_base64_decode(decoded, sizeof(decoded)-1, &olen,
                          (unsigned char*)(hdr+6), strlen(hdr+6));
    decoded[olen] = '\0';

    // decoded = "user:pass"
    char expected[128];
    snprintf(expected, sizeof(expected), "%s:%s",
             CONFIG_API_AUTH_USER, CONFIG_API_AUTH_PASS);
    if (strcmp((char*)decoded, expected) == 0) return true;
fail:
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"API\"");
    httpd_resp_sendstr(req, "{\"error\":\"Unauthorized\"}");
    return false;
}
```

### Token register handler (POST /token)
```
Body: {"ip":"192.168.1.10","token":"<apns-device-token>"}
```
Logic (in handler, before any NVS write):
1. Check block list — if `ip` exists in `"tok_blk"` → return `200 {"status":"ignored","reason":"blocked"}`
2. Check send list — if `ip` exists in `"tok_send"` AND stored token == incoming token → return `200 {"status":"ignored","reason":"no_change"}`
3. Otherwise → `token_store_send_set(ip, token)` → return `200 {"status":"ok"}`

No 4xx is returned for ignored cases — caller does not need to treat it as an error.

```c
// Pseudo-code inside token_register_handler:
char existing_token[100] = {0};

// 1. Blocked?
if (token_store_block_get(ip, existing_token, sizeof(existing_token)) == ESP_OK) {
    return send_json(req, "{\"status\":\"ignored\",\"reason\":\"blocked\"}");
}

// 2. Already identical in send list?
if (token_store_send_get(ip, existing_token, sizeof(existing_token)) == ESP_OK
    && strcmp(existing_token, token) == 0) {
    return send_json(req, "{\"status\":\"ignored\",\"reason\":\"no_change\"}");
}

// 3. Write
token_store_send_set(ip, token);
return send_json(req, "{\"status\":\"ok\"}");
```

### push handler — server_type parsing (POST /push)
```
Body: {
  "device_token": "...",
  "title": "...",
  "body": "...",
  "badge": 1,
  "sound": "default",
  "custom_payload": "...",   // optional
  "server_type": "sandbox"   // optional; "sandbox" (default) | "production"
}
```
```c
// Inside push_handler, after parsing existing fields:
const char *srv = cJSON_GetStringValue(cJSON_GetObjectItem(root, "server_type"));
p->use_sandbox = !(srv && strcmp(srv, "production") == 0);  // default sandbox

// In apns_send_task, replace direct use of g_apns_config:
apns_config_t cfg = g_apns_config;
cfg.use_sandbox = p->use_sandbox;
esp_err_t ret = apns_send_notification(&cfg, &notif);
```

### blast handler — server_type parsing (POST /blast)
```
Body: {
  "title": "...",
  "body": "...",
  "badge": 1,
  "sound": "default",        // optional
  "custom_payload": "...",   // optional
  "server_type": "sandbox"   // optional; "sandbox" (default) | "production"
}
```

### Blast handler task
```c
static void blast_task(void *arg) {
    blast_params_t *p = arg;

    // Build config copy with per-request server_type
    apns_config_t cfg = g_apns_config;
    cfg.use_sandbox = p->use_sandbox;

    token_entry_t entries[TOKEN_MAX_ENTRIES];
    size_t count = TOKEN_MAX_ENTRIES;
    token_store_send_list(entries, &count, TOKEN_MAX_ENTRIES);

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
        ESP_LOGI(TAG, "blast[%s]: %s", entries[i].ip, r == ESP_OK ? "ok" : "fail");
    }
    ESP_LOGI(TAG, "blast done: %d ok, %d fail (server=%s)",
             ok, fail, p->use_sandbox ? "sandbox" : "production");
    free(p);
    vTaskDelete(NULL);
}
```

### Note on APNs multi-token
APNs HTTP/2 API is **one request per device token** — there is no batch endpoint.
The blast task handles this by making sequential connections (ESP32 flash/RAM limits make parallel HTTP/2 connections impractical). For larger deployments, a cloud relay would be more appropriate.

---

## Implementation Steps

- [ ] 1. Add `API_AUTH_USER` / `API_AUTH_PASS` to `Kconfig.projbuild` + `sdkconfig.defaults`
- [ ] 2. Create `main/token_store.h` (types + function declarations)
- [ ] 3. Create `main/token_store.c` (NVS-backed implementation)
- [ ] 4. Update `main/CMakeLists.txt` (add token_store.c)
- [ ] 5. Update `main/scan.c` (call `token_store_init()`)
- [ ] 6. Rewrite `main/api_server.c` (auth helper + all new handlers + blast task)
- [ ] 7. Update `main/api_server.h` (doc comments)
- [ ] 8. Build & test

---

## Open Questions / Risks

| Issue | Decision |
|-------|----------|
| `apns_send_notification` uses static globals | Blast task calls sequentially — safe |
| NVS `list` requires `nvs_entry_find` (IDF v5+) | Verify IDF version in use |
| Device token length (APNs can be up to 100 hex chars) | Use `char token[100]` |
| IP as NVS key (max 15 chars) | IPv4 fits exactly |
| Memory: blast_params_t on heap | malloc/free in handler/task |
| `server_type` invalid / missing value | Default to `"sandbox"` silently |
| Global `g_apns_config` must not be mutated per-request | Use local `apns_config_t cfg = g_apns_config` copy |
| `POST /token` — IP already blocked | Skip NVS write, return `{"status":"ignored","reason":"blocked"}` |
| `POST /token` — IP+token identical in send list | Skip NVS write, return `{"status":"ignored","reason":"no_change"}` |
