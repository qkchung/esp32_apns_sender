# API Reference

All endpoints require **HTTP Basic Authentication**.

```
Authorization: Basic <base64("username:password")>
```

Credentials are configured in `sdkconfig.defaults`:
- `CONFIG_API_AUTH_USER` (default: `admin`)
- `CONFIG_API_AUTH_PASS` (default: `changeme`)

All requests and responses use `Content-Type: application/json`.

---

## Token Registration

### `POST /token`

Register or update a device push token keyed by IP address. Adds to the **send list**.

Write is skipped (returns `"ignored"`) if:
- The IP is already in the **block list**
- The exact same IP + token pair already exists in the send list

**Request body**
```json
{
  "ip": "192.168.1.10",
  "token": "<apns-device-token>"
}
```

**Responses**

| Condition | Body |
|-----------|------|
| Written successfully | `{"status":"ok"}` |
| IP is blocked | `{"status":"ignored","reason":"blocked"}` |
| Token unchanged | `{"status":"ignored","reason":"no_change"}` |
| Missing fields | `{"error":"Missing ip or token"}` |

**Example**
```bash
curl -u admin:changeme -X POST http://<device-ip>/token \
  -H "Content-Type: application/json" \
  -d '{"ip":"192.168.1.10","token":"abc123def456..."}'
```

---

## Send List

### `GET /tokens/send`

List all entries in the send list.

**Response**
```json
{
  "count": 2,
  "entries": [
    {"ip": "192.168.1.10", "token": "abc123..."},
    {"ip": "192.168.1.11", "token": "def456..."}
  ]
}
```

**Example**
```bash
curl -u admin:changeme http://<device-ip>/tokens/send
```

---

### `DELETE /tokens/send`

Remove an entry from the send list.

**Request body**
```json
{ "ip": "192.168.1.10" }
```

**Responses**

| Condition | Status | Body |
|-----------|--------|------|
| Removed | 200 | `{"status":"ok"}` |
| IP not found | 404 | `{"error":"IP not in send list"}` |

**Example**
```bash
curl -u admin:changeme -X DELETE http://<device-ip>/tokens/send \
  -H "Content-Type: application/json" \
  -d '{"ip":"192.168.1.10"}'
```

---

## Block List

### `GET /tokens/block`

List all entries in the block list.

**Response**
```json
{
  "count": 1,
  "entries": [
    {"ip": "192.168.1.99", "token": "xyz789..."}
  ]
}
```

**Example**
```bash
curl -u admin:changeme http://<device-ip>/tokens/block
```

---

### `POST /tokens/block`

Add or overwrite an entry directly in the block list. Use this to pre-emptively block a device that is not in the send list.

**Request body**
```json
{
  "ip": "192.168.1.99",
  "token": "<apns-device-token>"
}
```

**Responses**

| Condition | Status | Body |
|-----------|--------|------|
| Written | 200 | `{"status":"ok"}` |
| Missing fields | 400 | `{"error":"Missing ip or token"}` |

**Example**
```bash
curl -u admin:changeme -X POST http://<device-ip>/tokens/block \
  -H "Content-Type: application/json" \
  -d '{"ip":"192.168.1.99","token":"xyz789..."}'
```

---

### `DELETE /tokens/block`

Remove an entry from the block list.

**Request body**
```json
{ "ip": "192.168.1.99" }
```

**Responses**

| Condition | Status | Body |
|-----------|--------|------|
| Removed | 200 | `{"status":"ok"}` |
| IP not found | 404 | `{"error":"IP not in block list"}` |

**Example**
```bash
curl -u admin:changeme -X DELETE http://<device-ip>/tokens/block \
  -H "Content-Type: application/json" \
  -d '{"ip":"192.168.1.99"}'
```

---

## Move Between Lists

### `POST /tokens/move-to-block`

Move an existing entry from the **send list** to the **block list**. Fails if the IP is not in the send list.

**Request body**
```json
{ "ip": "192.168.1.10" }
```

**Responses**

| Condition | Status | Body |
|-----------|--------|------|
| Moved | 200 | `{"status":"ok"}` |
| IP not in send list | 404 | `{"error":"IP not in send list"}` |

**Example**
```bash
curl -u admin:changeme -X POST http://<device-ip>/tokens/move-to-block \
  -H "Content-Type: application/json" \
  -d '{"ip":"192.168.1.10"}'
```

---

### `POST /tokens/move-to-send`

Move an existing entry from the **block list** to the **send list**. Fails if the IP is not in the block list.

**Request body**
```json
{ "ip": "192.168.1.99" }
```

**Responses**

| Condition | Status | Body |
|-----------|--------|------|
| Moved | 200 | `{"status":"ok"}` |
| IP not in block list | 404 | `{"error":"IP not in block list"}` |

**Example**
```bash
curl -u admin:changeme -X POST http://<device-ip>/tokens/move-to-send \
  -H "Content-Type: application/json" \
  -d '{"ip":"192.168.1.99"}'
```

---

## Push Notifications

### `POST /push`

Send a push notification to a **single explicit device token**. The request returns immediately (`"queued"`) and the notification is sent in the background.

**Request body**

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `device_token` | string | Yes | APNs device token |
| `title` | string | Yes | Notification title |
| `body` | string | Yes | Notification body text |
| `badge` | integer | No | Badge count (omit field to not set badge) |
| `sound` | string | No | Sound name, e.g. `"default"` |
| `custom_payload` | string | No | Raw JSON fields merged at root level |
| `server_type` | string | No | `"sandbox"` (default) or `"production"` |

```json
{
  "device_token": "abc123def456...",
  "title": "Hello",
  "body": "World",
  "badge": 1,
  "sound": "default",
  "custom_payload": "\"type\":\"alert\",\"id\":42",
  "server_type": "sandbox"
}
```

**Response**
```json
{"status":"queued"}
```

**Example**
```bash
curl -u admin:changeme -X POST http://<device-ip>/push \
  -H "Content-Type: application/json" \
  -d '{
    "device_token": "abc123def456...",
    "title": "Test",
    "body": "Hello from ESP32",
    "custom_payload": "\"type\":\"alert\"",
    "server_type": "sandbox"
  }'
```

---

### `POST /blast`

Send the **same push notification to every token in the send list**. Tokens in the block list are never included. The request returns immediately (`"queued"`); notifications are sent sequentially in the background. Per-token results are logged to the device console.

**Request body**

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `title` | string | Yes | Notification title |
| `body` | string | Yes | Notification body text |
| `badge` | integer | No | Badge count |
| `sound` | string | No | Sound name, e.g. `"default"` |
| `custom_payload` | string | No | Raw JSON fields merged at root level |
| `server_type` | string | No | `"sandbox"` (default) or `"production"` |

```json
{
  "title": "Announcement",
  "body": "Motion detected at front door",
  "badge": 1,
  "sound": "default",
  "custom_payload": "\"type\":\"alert\"",
  "server_type": "production"
}
```

**Response**
```json
{"status":"queued"}
```

**Example**
```bash
curl -u admin:changeme -X POST http://<device-ip>/blast \
  -H "Content-Type: application/json" \
  -d '{
    "title": "Alert",
    "body": "Motion detected",
    "sound": "default",
    "custom_payload": "\"type\":\"alert\"",
    "server_type": "production"
  }'
```

---

## Error Responses

All errors return a JSON body with an `"error"` field.

| HTTP Status | Meaning |
|-------------|---------|
| `401 Unauthorized` | Missing or invalid `Authorization` header |
| `400 Bad Request` | Missing or malformed JSON body / required field absent |
| `404 Not Found` | IP not found in the target list |
| `500 Internal Server Error` | NVS write failure or FreeRTOS task creation failure |

```json
{"error":"Missing ip or token"}
```

---

## `server_type` Field

Both `/push` and `/blast` accept an optional `"server_type"` field that selects the APNs endpoint for that request only, regardless of the compile-time `CONFIG_APNS_USE_SANDBOX` setting.

| Value | APNs endpoint |
|-------|---------------|
| `"sandbox"` *(default)* | `api.sandbox.push.apple.com` |
| `"production"` | `api.push.apple.com` |

If the field is omitted or contains any other value, `"sandbox"` is used.

---

## Quick-Reference Table

| Method | URI | Auth | Description |
|--------|-----|------|-------------|
| POST | `/token` | Yes | Register / update device token → send list |
| GET | `/tokens/send` | Yes | List send list |
| DELETE | `/tokens/send` | Yes | Remove from send list |
| GET | `/tokens/block` | Yes | List block list |
| POST | `/tokens/block` | Yes | Add directly to block list |
| DELETE | `/tokens/block` | Yes | Remove from block list |
| POST | `/tokens/move-to-block` | Yes | Move send → block |
| POST | `/tokens/move-to-send` | Yes | Move block → send |
| POST | `/push` | Yes | Single-token push notification |
| POST | `/blast` | Yes | Broadcast push to entire send list |
