# apns_pusher

An ESP32 firmware that acts as a self-hosted Apple Push Notification (APNs) gateway. It connects to WiFi, exposes a REST API over simple auth HTTP, and pushes notifications to iOS devices directly via Apple's APNs HTTP/2 API — with no cloud backend required. Free of saas subscription, de-cloud , 0.2w power saving, being iot component sending instant alert to ios clients ( no more slow ifttt or homekit )

This is a code demo for push service for lan iot project, highly recommend you harden security in your project, never expose this endpoint directly to internet
I assumed lan devices are static assign with fixed ip , so ip is used as key here 

## Features

- **Token-based APNs auth** — generates ES256 JWTs from your `.p8` key using mbedTLS; no certificate-based auth needed
- **HTTP/2 to Apple** — uses `sh2lib` (nghttp2 wrapper) for direct APNs connections with TLS verified against Apple's CA bundle
- **REST API** — HTTP Basic Auth protected endpoints for sending notifications and managing device tokens
- **Persistent token store** — NVS-backed send/block lists, keyed by device IP; survives reboots
- **Broadcast** — blast the same notification to every registered device in one call
- **Sandbox / Production** — selectable per-request via `server_type` field
- **Up to 3 concurrent pushes** — semaphore-guarded background FreeRTOS tasks

## Hardware

Any ESP32 variant supported by ESP-IDF with enough RAM for TLS + HTTP/2 (ESP32, ESP32-S3, etc.).

| Supported Targets | ESP32 | ESP32-S2 | ESP32-S3 | ESP32-C3 | ESP32-C6 |
|---|---|---|---|---|---|

## Prerequisites

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/) v5.x
- An [Apple Developer](https://developer.apple.com) account
- An APNs authentication key (`.p8` file) created in the Apple Developer portal under **Certificates, Identifiers & Profiles → Keys**

## Getting Started

### 1. Clone the repository

```bash
git clone https://github.com/your-username/apns_pusher.git
cd apns_pusher
```

### 2. Add your APNs key

Copy your `.p8` key file into the certs directory with the exact filename the build system expects:

```
main/certs/apns_auth_key.p8
```

> **Never commit this file.** It is listed in `.gitignore` via the `*.p8` rule.

### 3. Create your local config file

```bash
# Linux / macOS
cp .sdkconfig.defaults sdkconfig.defaults

# Windows (PowerShell)
Copy-Item .sdkconfig.defaults sdkconfig.defaults
```

Edit `sdkconfig.defaults` with your real values, then run menuconfig to review:

```bash
idf.py menuconfig
```

Navigate to **APNs Configuration** and fill in:

| Setting | Description |
|---|---|
| WiFi SSID | Your WiFi network name |
| WiFi Password | Your WiFi password |
| Apple Team ID | 10-character Team ID from the Apple Developer portal |
| APNs Key ID | 10-character Key ID of your `.p8` authentication key |
| App Bundle ID | Bundle identifier of your iOS app (e.g. `com.example.myapp`) |
| Use APNs Sandbox | Enable for development builds, disable for production |
| HTTP API username | Username for Basic Auth on all API endpoints |
| HTTP API password | Password for Basic Auth — **change from the default** |

> `sdkconfig` and `sdkconfig.defaults` are git-ignored. No credentials are committed.

### 4. Build and flash

```bash
idf.py build
idf.py -p PORT flash monitor
```

On first boot the device connects to WiFi, syncs time via NTP, and starts the HTTP API server. The assigned IP address is printed in the serial monitor.

## REST API

All endpoints require **HTTP Basic Authentication** using the credentials configured above.

### Send a push notification

```
POST /push
```

```json
{
  "device_token":   "abc123...64hexchars",
  "title":          "Hello",
  "body":           "World",
  "badge":          1,
  "sound":          "default",
  "custom_payload": "\"category\":\"alert\"",
  "server_type":    "sandbox"
}
```

`badge`, `sound`, `custom_payload`, and `server_type` are optional. `server_type` defaults to `"sandbox"`.

**Response:** `{"status":"queued"}` — the push is dispatched asynchronously in a background task.

---

### Broadcast to all registered devices

```
POST /blast
```

Same body as `/push` but without `device_token`. Sends the same notification to every token in the send list for the given `server_type`.

**Response:** `{"status":"queued"}`

---

### Device Token Management

Tokens are stored by **(server_type, device IP)** in NVS flash. Two lists are maintained: **send** (active recipients) and **block** (opt-out / suppressed).

| Method | Endpoint | Description |
|---|---|---|
| `POST` | `/token` | Register or update a device token |
| `GET` | `/tokens/send` | List all send-list entries |
| `DELETE` | `/tokens/send` | Remove an entry from the send list |
| `GET` | `/tokens/block` | List all block-list entries |
| `POST` | `/tokens/block` | Add an entry directly to the block list |
| `DELETE` | `/tokens/block` | Remove an entry from the block list |
| `POST` | `/tokens/move-to-block` | Move an IP from send → block list |
| `POST` | `/tokens/move-to-send` | Move an IP from block → send list |

#### Register a token

```http
POST /token
Content-Type: application/json

{"ip":"192.168.1.42","token":"<apns-device-token>","server_type":"sandbox"}
```

Response: `{"status":"ok"}` | `{"status":"ignored","reason":"blocked"}` | `{"status":"ignored","reason":"no_change"}`

#### List / delete from send list

```http
GET  /tokens/send
DELETE /tokens/send    {"ip":"192.168.1.42"}
```

#### Move between lists

```http
POST /tokens/move-to-block   {"ip":"192.168.1.42"}
POST /tokens/move-to-send    {"ip":"192.168.1.99"}
```

---

## Example: curl

```bash
ESP_IP=192.168.1.10
AUTH="admin:your-password"

# Send a single push notification (sandbox)
curl -u "$AUTH" -X POST http://$ESP_IP/push \
  -H "Content-Type: application/json" \
  -d '{"device_token":"YOUR_64_CHAR_DEVICE_TOKEN","title":"Test","body":"Hello from ESP32","server_type":"sandbox"}'

# Register a device token
curl -u "$AUTH" -X POST http://$ESP_IP/token \
  -H "Content-Type: application/json" \
  -d '{"ip":"192.168.1.42","token":"YOUR_TOKEN","server_type":"sandbox"}'

# Blast to all registered sandbox devices
curl -u "$AUTH" -X POST http://$ESP_IP/blast \
  -H "Content-Type: application/json" \
  -d '{"title":"Broadcast","body":"Hi everyone","server_type":"sandbox"}'

# List all registered tokens
curl -u "$AUTH" http://$ESP_IP/tokens/send
```

## Project Structure

```
apns_pusher/
├── main/
│   ├── apns.c / apns.h            # APNs client: JWT ES256 + HTTP/2 POST
│   ├── api_server.c / api_server.h # HTTP REST API with Basic Auth
│   ├── token_store.c / token_store.h # NVS-backed send/block token registry
│   ├── scan.c                     # app_main: WiFi init, NTP sync, startup
│   ├── Kconfig.projbuild          # idf.py menuconfig settings
│   ├── CMakeLists.txt
│   └── certs/
│       └── apns_auth_key.p8       # ← place your .p8 key here (git-ignored)
├── .sdkconfig.defaults            # sanitised config template (safe to commit)
├── .gitignore
└── CMakeLists.txt
```

## Security Notes

- The `.p8` private key is embedded into the firmware binary at compile time. It is **never committed** to git (covered by the `*.p8` `.gitignore` rule).
- All runtime secrets (WiFi password, APNs IDs, API credentials) live in `sdkconfig`, which is also git-ignored.
- The HTTP API has no TLS — use it on a trusted local network only, or place the device behind a TLS-terminating reverse proxy.
- **Change the default API password** (`changeme`) before deploying.


