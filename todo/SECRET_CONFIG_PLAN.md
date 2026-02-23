# Plan: Replace Hardcoded Secrets with Runtime-Configurable NVS Storage

## Background

All secrets in this ESP-IDF project are currently baked in at compile time via
`sdkconfig.defaults` and `EMBED_TXTFILES`. The goal is to move everything to
**NVS flash** (already used for token storage), configurable over a provisioning
API — no recompile needed per deployment.

### Exposed Secrets (as of audit)

| Secret | File | Severity |
|--------|------|----------|
| WiFi SSID / Password | `sdkconfig.defaults:12-13` | HIGH |
| APNs Team ID / Key ID | `sdkconfig.defaults:19-20` | HIGH |
| API username / password | `sdkconfig.defaults:28-29` | HIGH |
| APNs EC private key (.p8) | `main/certs/apns_auth_key.p8` | CRITICAL |

---

## Architecture Overview

```
First boot (NVS empty)
       │
       ▼
 SoftAP mode ("ESP-SCAN-SETUP")
  └─ Serves /provision HTML form
       │
       ▼
 User POSTs credentials via browser or curl
  └─ WiFi creds    → NVS
  └─ APNs creds    → NVS
  └─ APNs .p8 key  → NVS
  └─ API user/pass → NVS
       │
       ▼
 Reboot → Station mode (normal operation)
  └─ Reads all config from NVS at startup
```

---

## Phase 1 — NVS Config Module (`config_store.c/.h`)

Create a dedicated NVS namespace `"app_config"` with get/set helpers for every secret.

### NVS Key Mapping

| NVS Key        | Type   | Replaces                    |
|----------------|--------|-----------------------------|
| `wifi_ssid`    | string | `CONFIG_APNS_WIFI_SSID`     |
| `wifi_pass`    | string | `CONFIG_APNS_WIFI_PASSWORD` |
| `apns_team_id` | string | `CONFIG_APNS_TEAM_ID`       |
| `apns_key_id`  | string | `CONFIG_APNS_KEY_ID`        |
| `apns_bundle_id` | string | `CONFIG_APNS_BUNDLE_ID`   |
| `apns_sandbox` | u8     | `CONFIG_APNS_USE_SANDBOX`   |
| `apns_key_pem` | blob   | embedded `.p8` file         |
| `api_user`     | string | `CONFIG_API_AUTH_USER`      |
| `api_pass`     | string | `CONFIG_API_AUTH_PASS`      |

### Functions to Implement

```c
esp_err_t config_store_init(void);
bool      config_store_is_provisioned(void);

esp_err_t config_store_get_wifi(char *ssid, size_t ssid_len,
                                char *pass, size_t pass_len);
esp_err_t config_store_set_wifi(const char *ssid, const char *pass);

esp_err_t config_store_get_apns(apns_config_t *out);
esp_err_t config_store_set_apns(const char *team_id, const char *key_id,
                                const char *bundle_id, bool sandbox);

esp_err_t config_store_get_key_pem(char *buf, size_t *len);
esp_err_t config_store_set_key_pem(const char *pem, size_t len);

esp_err_t config_store_get_api_auth(char *user, size_t user_len,
                                    char *pass, size_t pass_len);
esp_err_t config_store_set_api_auth(const char *user, const char *pass);
```

---

## Phase 2 — First-Boot Provisioning (SoftAP + captive portal)

### Modified `app_main()` Boot Logic (`main/scan.c`)

```
app_main()
  ├─ nvs_flash_init()
  ├─ config_store_init()
  ├─ if (!config_store_is_provisioned())
  │     wifi_start_softap("ESP-SCAN-SETUP")   ← new
  │     provision_server_start()              ← new
  │     wait for provisioning_done event
  │     esp_restart()
  └─ else
        wifi_init_sta()   ← reads WiFi creds from NVS
        sync_time()
        config_store_get_apns(&g_apns_config)
        config_store_get_key_pem(...)
        apns_init()
        api_server_start()
```

### New File: `main/provision_server.c`

Serves a minimal HTTP server on SoftAP interface:

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | HTML form with fields for all secrets |
| `/provision` | POST | Accepts JSON body, writes to NVS, fires restart event |

The `.p8` key is uploaded as a `multipart/form-data` file field. mbedtls parses
it from NVS at runtime, the same way it currently parses from the embedded binary.

---

## Phase 3 — Update Existing Code to Read from NVS

### `main/scan.c`

- **Remove** `extern const char apns_auth_key_start[]` / `apns_auth_key_end[]` declarations
- **Remove** `CONFIG_APNS_WIFI_SSID`, `CONFIG_APNS_WIFI_PASSWORD` from `wifi_init_sta()`
- **Replace** with `config_store_get_wifi(&ssid, &pass)`
- **Remove** `CONFIG_APNS_TEAM_ID`, `CONFIG_APNS_KEY_ID`, `CONFIG_APNS_BUNDLE_ID` from `app_main()`
- **Replace** with `config_store_get_apns(&g_apns_config)` and `config_store_get_key_pem(...)`

### `main/api_server.c`

- **Remove** `CONFIG_API_AUTH_USER`, `CONFIG_API_AUTH_PASS` from `auth_check()` (line 85-86)
- **Replace** with `config_store_get_api_auth(&user, &pass)`

### `main/CMakeLists.txt`

- **Remove** `EMBED_TXTFILES "certs/apns_auth_key.p8"` — key loaded from NVS at runtime
- **Add** `config_store.c` and `provision_server.c` to `SRCS`

---

## Phase 4 — Runtime Re-configuration API (optional)

Add protected `PUT /config` endpoints to `api_server.c` for updating credentials
without re-entering provisioning mode. All endpoints require HTTP Basic Auth.

| Endpoint | Body | Effect |
|----------|------|--------|
| `PUT /config/wifi` | `{"ssid":"...","password":"..."}` | Updates WiFi, restarts |
| `PUT /config/apns` | `{"team_id":"...","key_id":"...","bundle_id":"...","sandbox":true}` | Updates APNs metadata |
| `PUT /config/apns/key` | multipart `.p8` file upload | Replaces stored private key |
| `PUT /config/auth` | `{"user":"...","pass":"..."}` | Updates API credentials |

Changes to WiFi take effect on next reboot. APNs/auth changes take effect immediately.

---

## Phase 5 — Cleanup

1. **`sdkconfig.defaults`** — remove all secret values; keep only flash/partition settings
2. **`main/certs/apns_auth_key.p8`** — delete from repo
3. **`main/certs/apns_auth_key.p8.bak`** — delete from repo
4. **`.gitignore`** — add:
   ```
   sdkconfig
   sdkconfig.defaults
   main/certs/
   build/
   ```
5. **Purge git history** using `git filter-repo` to remove the key from all past commits:
   ```bash
   git filter-repo --path main/certs/apns_auth_key.p8 --invert-paths
   git filter-repo --path sdkconfig.defaults --invert-paths
   ```
6. **Rotate all exposed credentials**:
   - Revoke and regenerate APNs key in Apple Developer Portal
   - Change WiFi password on the router
   - Set a strong API password during first-boot provisioning

---

## File Change Summary

| File | Action |
|------|--------|
| `main/config_store.c` | **New** — NVS get/set for all secrets |
| `main/config_store.h` | **New** — public API header |
| `main/provision_server.c` | **New** — SoftAP + HTML form + POST /provision handler |
| `main/provision_server.h` | **New** — header |
| `main/scan.c` | **Modify** — read WiFi/APNs from NVS; add provisioning boot path |
| `main/api_server.c` | **Modify** — read API auth from NVS; add PUT /config endpoints |
| `main/CMakeLists.txt` | **Modify** — remove `EMBED_TXTFILES`, add new source files |
| `sdkconfig.defaults` | **Modify** — strip all secret values |
| `.gitignore` | **Modify** — add `sdkconfig`, `main/certs/`, `build/` |
| `main/certs/apns_auth_key.p8` | **Delete** |
| `main/certs/apns_auth_key.p8.bak` | **Delete** |
