# Reset & Error Handling in SoftAP Provisioning Mode

## Overview

This document covers two concerns that were not addressed in `SECRET_CONFIG_PLAN.md`:

1. **How to trigger a factory reset** when settings go wrong after provisioning
2. **How to handle errors that occur during SoftAP provisioning** (before normal boot)

Both are grounded in the existing code in `main/scan.c` and `main/api_server.c`.

---

## Part 1 — Factory Reset: How to Re-Enter Provisioning

### When is a reset needed?

| Situation | Example |
|-----------|---------|
| Wrong WiFi credentials saved | Device boots, WiFi connect fails every time → stuck |
| Wrong APNs key saved | APNs send returns JWT parse error on every attempt |
| New deployment, different WiFi | Moving the device to another location |
| Forgotten API password | Can no longer reach any endpoint |
| Corrupted NVS | NVS returns errors on read |

### Mechanism 1 — GPIO Button Hold (Recommended)

Wire a physical button to a GPIO pin (e.g. GPIO0, which is the BOOT button on most ESP32 devkits). At startup, sample the pin. If held for ≥3 seconds, erase the `"app_config"` NVS namespace and restart into SoftAP mode.

```
app_main()
  ├─ nvs_flash_init()
  ├─ config_store_init()
  ├─ check_factory_reset_button()   ← NEW, runs before WiFi
  │     gpio_set_direction(GPIO_NUM_0, GPIO_MODE_INPUT)
  │     if gpio_get_level == 0 for 3 s:
  │         ESP_LOGW(TAG, "Factory reset triggered by button")
  │         config_store_erase_all()   ← erases "app_config" namespace only
  │         esp_restart()
  └─ ... rest of boot
```

> **Why GPIO0?** It is already connected to a button on every ESP32 devkit. It is pulled HIGH internally; pressing the button pulls it LOW.

**`config_store_erase_all()`** must erase only the `"app_config"` NVS namespace, not the `"tok_snd_s"` / `"tok_snd_p"` / `"tok_blk_s"` / `"tok_blk_p"` token namespaces, so registered device tokens survive a reset.

```c
esp_err_t config_store_erase_all(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open("app_config", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
    return err;
}
```

### Mechanism 2 — API Endpoint `POST /factory-reset`

Add a protected endpoint to `api_server.c` for remote reset when the device is reachable but misconfigured (e.g. wrong APNs key but WiFi still works).

```
POST /factory-reset
Authorization: Basic <base64 user:pass>
Body: {"confirm": true}

→ 200 {"status": "resetting"}
→ device erases app_config NVS, restarts into SoftAP mode
```

This reuses the existing `auth_check()` function at [api_server.c:64](../main/api_server.c#L64), so the user must know the current API password to trigger it.

### Mechanism 3 — Watchdog Auto-Reset in SoftAP Mode

If the device is stuck in SoftAP mode and no one completes provisioning within a timeout (e.g. 10 minutes), restart and try again. This prevents the device from hanging indefinitely if provisioning is interrupted.

```c
/* In provision_server.c — after starting SoftAP */
#define PROVISION_TIMEOUT_MS  (10 * 60 * 1000)   /* 10 minutes */

xTaskCreate(provision_watchdog_task, "prov_wdog", 2048, NULL, 3, NULL);

static void provision_watchdog_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(PROVISION_TIMEOUT_MS));
    ESP_LOGW(TAG, "Provisioning timeout — restarting");
    esp_restart();
}
/* Cancel this task by deleting it when POST /provision succeeds */
```

---

## Part 2 — Error Handling in SoftAP Provisioning

### Error categories

```
SoftAP provisioning errors
├── A. HTTP server / network errors    (SoftAP fails to start, client disconnects)
├── B. Input validation errors         (missing fields, wrong format)
├── C. Semantic validation errors      (WiFi creds wrong, key fails to parse)
└── D. NVS write errors                (flash full, corrupted)
```

---

### A. HTTP Server / Network Errors

#### A1 — SoftAP fails to start

In `wifi_start_softap()`, if `esp_wifi_start()` returns an error, the device cannot be provisioned wirelessly. Log the error and restart after a delay.

```c
esp_err_t wifi_start_softap(const char *ssid) {
    /* ... init netif, wifi config ... */
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SoftAP start failed: %s — restarting in 5s", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }
    return ESP_OK;
}
```

#### A2 — Client disconnects mid-POST

`esp_http_server` returns a partial body. The existing `read_body()` helper at [api_server.c:99](../main/api_server.c#L99) reads once and returns the byte count. In `provision_server.c`, use the same pattern but check the return value:

```c
int received = httpd_req_recv(req, buf, len - 1);
if (received <= 0) {
    /* HTTPD_SOCK_ERR_TIMEOUT or connection closed */
    ESP_LOGW(TAG, "Provision POST: client disconnected (received=%d)", received);
    /* Nothing was written to NVS yet — no cleanup needed */
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"error\":\"Connection closed\"}");
    return ESP_OK;
}
```

No NVS rollback is needed here because writes only happen after full validation (see C below).

---

### B. Input Validation Errors

Validate every field before touching NVS. Return a clear error message with which field failed.

| Field | Validation rule |
|-------|----------------|
| `wifi_ssid` | Non-empty, ≤ 32 bytes (IEEE 802.11 limit) |
| `wifi_pass` | 8–63 bytes for WPA2, or exactly 0 for open network |
| `apns_team_id` | Exactly 10 alphanumeric characters |
| `apns_key_id` | Exactly 10 alphanumeric characters |
| `apns_bundle_id` | Non-empty, contains at least one `.` |
| `apns_key_pem` | Starts with `-----BEGIN PRIVATE KEY-----` |
| `api_user` | Non-empty, ≤ 32 bytes |
| `api_pass` | ≥ 8 bytes, ≤ 64 bytes |

Example validation response:

```json
HTTP/1.1 400 Bad Request
{"error": "Validation failed", "field": "apns_team_id", "reason": "Must be exactly 10 characters"}
```

---

### C. Semantic Validation Errors

#### C1 — WiFi credentials wrong

You cannot verify WiFi credentials from SoftAP mode (the radio is in AP mode, not STA mode). The verification must happen **after** the device reboots into STA mode.

**Strategy: two-stage boot with rollback**

```
boot (provisioned)
  ├─ Read WiFi creds from NVS
  ├─ wifi_init_sta()  — attempt connection, max 5 retries (existing logic: scan.c:45-50)
  ├─ if WIFI_FAIL_BIT set after retries:
  │     ESP_LOGE(TAG, "WiFi failed — falling back to SoftAP for re-provisioning")
  │     config_store_erase_all()   ← clear only wifi_ssid/wifi_pass, not APNs or API creds
  │     esp_restart()              ← will enter SoftAP mode again
  └─ else: continue normal boot
```

The existing `WIFI_FAIL_BIT` logic in [scan.c:50](../main/scan.c#L50) already sets this bit. The only change needed is to act on it instead of `return` (which currently just silently stops).

Change this at [scan.c:141-143](../main/scan.c#L141):
```c
/* Current code */
if (wifi_init_sta() != ESP_OK) {
    return;   // ← silent stop, device is bricked
}

/* New behaviour */
if (wifi_init_sta() != ESP_OK) {
    ESP_LOGE(TAG, "WiFi connection failed — clearing WiFi config, re-entering provisioning");
    config_store_clear_wifi();   // erases only wifi_ssid + wifi_pass from NVS
    esp_restart();
}
```

#### C2 — APNs private key fails to parse

Validate the key **at provisioning time**, before saving to NVS. Attempt an mbedtls parse in the `/provision` handler itself. If it fails, return an error without writing anything.

```c
/* In POST /provision handler, before any nvs_set calls */
mbedtls_pk_context pk;
mbedtls_pk_init(&pk);
int rc = mbedtls_pk_parse_key(&pk,
    (const unsigned char *)pem_buf, strlen(pem_buf) + 1,
    NULL, 0, mbedtls_ctr_drbg_random, &ctr_drbg);
mbedtls_pk_free(&pk);

if (rc != 0) {
    ESP_LOGE(TAG, "APNs key parse failed: -0x%04x", (unsigned)-rc);
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"error\":\"Invalid APNs private key (.p8 parse failed)\"}");
    return ESP_OK;
}
/* Only reach here if key is valid — safe to write to NVS */
```

#### C3 — API password too weak (optional)

Reject passwords shorter than 8 characters at provisioning time. A brute-forced API password would allow any attacker on the WiFi network to send push notifications to all registered tokens.

---

### D. NVS Write Errors

NVS writes can fail if flash is full (`ESP_ERR_NVS_NOT_ENOUGH_SPACE`) or the partition is corrupted. Use **all-or-nothing write order** to avoid half-written state:

```
Writing order in POST /provision:
  1. Validate ALL fields (no NVS writes yet)
  2. config_store_set_key_pem()       ← largest blob first
  3. config_store_set_wifi()
  4. config_store_set_apns()
  5. config_store_set_api_auth()
  6. nvs_commit()                     ← single commit at the end
  7. if ANY step fails → log error, return 500, do NOT restart
```

On failure at any step, the provisioning flag (`wifi_ssid` key) remains unset, so the next reboot will return to SoftAP mode cleanly.

```c
esp_err_t err;
err = config_store_set_key_pem(pem, pem_len);
if (err != ESP_OK) goto nvs_fail;

err = config_store_set_wifi(ssid, pass);
if (err != ESP_OK) goto nvs_fail;

/* ... other sets ... */

httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Rebooting...\"}");
vTaskDelay(pdMS_TO_TICKS(500));   /* give HTTP response time to send */
esp_restart();
return ESP_OK;

nvs_fail:
    ESP_LOGE(TAG, "NVS write failed: %s", esp_err_to_name(err));
    /* Partial writes are orphaned — they will be overwritten on next provisioning */
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, "{\"error\":\"Flash write failed — try again\"}");
    return ESP_OK;
```

---

## Summary: Error → Recovery Table

| Error | When | Recovery |
|-------|------|----------|
| SoftAP fails to start | Provisioning boot | Log + restart after 5s |
| Client disconnects mid-POST | Provisioning | Return 400, no NVS written, stay in SoftAP |
| Missing/invalid form field | Provisioning | Return 400 with field name, stay in SoftAP |
| APNs key fails mbedtls parse | Provisioning | Return 400, no NVS written, stay in SoftAP |
| NVS write fails | Provisioning | Return 500, partial data orphaned, stay in SoftAP |
| WiFi creds wrong (connect fails) | Normal boot | Clear wifi keys from NVS, restart into SoftAP |
| APNs JWT rejected by Apple | Normal operation | `PUT /config/apns/key` to update key without re-provisioning |
| Forgotten API password | Normal operation | GPIO button hold → factory reset → re-provisioning |
| Device fully misconfigured | Any time | GPIO button hold → factory reset |
| Provisioning abandoned (timeout) | SoftAP mode | Watchdog restarts after 10 minutes |

---

## Update to `SECRET_CONFIG_PLAN.md`

Add these items to Phase 2 and Phase 3 of the existing plan:

- **Phase 2** additions:
  - `check_factory_reset_button()` called at the top of `app_main()` before any WiFi init
  - Provisioning watchdog task with 10-minute timeout
  - All-or-nothing NVS write order in `POST /provision`
  - APNs key validation via mbedtls before saving

- **Phase 3** additions:
  - WiFi failure in `wifi_init_sta()` triggers `config_store_clear_wifi()` + `esp_restart()` instead of silent `return`
  - New `POST /factory-reset` endpoint in `api_server.c`

- **New file**: `main/config_store.c` must expose `config_store_erase_all()` and `config_store_clear_wifi()`
