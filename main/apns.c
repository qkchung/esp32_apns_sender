/*
 * APNs (Apple Push Notification service) client implementation
 *
 * - JWT ES256 token generation using mbedtls
 * - HTTP/2 POST to APNs using sh2lib (nghttp2 wrapper)
 * - Certificate bundle verification for Apple's TLS certificates
 */

#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_crt_bundle.h"
#include "sh2lib.h"

#include "mbedtls/pk.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/sha256.h"
#include "mbedtls/base64.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"

#include "cJSON.h"
#include "apns.h"

static const char *TAG = "apns";

static SemaphoreHandle_t s_apns_mutex = NULL;

/* JWT cache — reuse for up to 55 min to avoid Apple's TooManyProviderTokenUpdates (1 h limit) */
#define JWT_VALID_SECONDS  3300
static char   s_jwt_cache[1024] = {0};
static time_t s_jwt_generated_at = 0;

esp_err_t apns_init(void)
{
    s_apns_mutex = xSemaphoreCreateMutex();
    return s_apns_mutex ? ESP_OK : ESP_FAIL;
}

#define APNS_HOST_PRODUCTION "api.push.apple.com"
#define APNS_HOST_SANDBOX    "api.sandbox.push.apple.com"

/* ------------------------------------------------------------------ */
/*  Base64URL helpers                                                  */
/* ------------------------------------------------------------------ */

/**
 * Standard base64 encode, then convert to URL-safe variant:
 *   '+' -> '-',  '/' -> '_',  trailing '=' removed
 */
static size_t base64url_encode(const unsigned char *src, size_t slen,
                               char *dst, size_t dlen)
{
    size_t olen = 0;
    int rc = mbedtls_base64_encode((unsigned char *)dst, dlen, &olen, src, slen);
    if (rc != 0) {
        return 0;
    }
    /* URL-safe replacements */
    for (size_t i = 0; i < olen; i++) {
        if (dst[i] == '+')      dst[i] = '-';
        else if (dst[i] == '/') dst[i] = '_';
    }
    /* Strip trailing '=' padding */
    while (olen > 0 && dst[olen - 1] == '=') {
        olen--;
    }
    dst[olen] = '\0';
    return olen;
}

/* ------------------------------------------------------------------ */
/*  DER -> raw (r || s) ECDSA signature conversion                     */
/* ------------------------------------------------------------------ */

/**
 * mbedtls produces ECDSA signatures in ASN.1/DER format:
 *   30 <len> 02 <rlen> <r> 02 <slen> <s>
 *
 * JWT ES256 expects the raw 64-byte format: r (32 bytes) || s (32 bytes)
 */
static int der_sig_to_raw(const unsigned char *der, size_t der_len,
                          unsigned char raw[64])
{
    if (der_len < 8 || der[0] != 0x30) {
        return -1;
    }

    size_t pos = 2; /* skip SEQUENCE tag + length */

    /* --- R --- */
    if (der[pos] != 0x02) return -1;
    pos++;
    size_t r_len = der[pos++];
    const unsigned char *r = &der[pos];
    pos += r_len;

    /* --- S --- */
    if (pos >= der_len || der[pos] != 0x02) return -1;
    pos++;
    size_t s_len = der[pos++];
    const unsigned char *s = &der[pos];

    /* Copy right-aligned into 32-byte slots */
    memset(raw, 0, 64);
    if (r_len > 32) { r += (r_len - 32); r_len = 32; }
    memcpy(raw + (32 - r_len), r, r_len);

    if (s_len > 32) { s += (s_len - 32); s_len = 32; }
    memcpy(raw + 32 + (32 - s_len), s, s_len);

    return 0;
}

/* ------------------------------------------------------------------ */
/*  JWT ES256 token generation                                         */
/* ------------------------------------------------------------------ */

/**
 * Generate a JWT signed with ES256 for APNs token-based authentication.
 *
 * Header : {"alg":"ES256","kid":"<key_id>"}
 * Payload: {"iss":"<team_id>","iat":<unix_timestamp>}
 */
static esp_err_t generate_jwt(const apns_config_t *config,
                              char *jwt_buf, size_t jwt_buf_len)
{
    esp_err_t ret = ESP_FAIL;
    mbedtls_pk_context pk;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;

    mbedtls_pk_init(&pk);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);

    /* Seed the DRBG */
    int rc = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                   (const unsigned char *)"apns_jwt", 8);
    if (rc != 0) {
        ESP_LOGE(TAG, "DRBG seed failed: -0x%04x", (unsigned)-rc);
        goto cleanup;
    }

    /* Parse the .p8 private key (PEM PKCS#8 EC P-256) */
    rc = mbedtls_pk_parse_key(&pk,
                              (const unsigned char *)config->apns_key_pem,
                              strlen(config->apns_key_pem) + 1,   /* PEM needs null */
                              NULL, 0,
                              mbedtls_ctr_drbg_random, &ctr_drbg);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to parse .p8 key: -0x%04x", (unsigned)-rc);
        goto cleanup;
    }

    /* --- Build JWT header & payload --- */
    char header[80];
    snprintf(header, sizeof(header),
             "{\"alg\":\"ES256\",\"kid\":\"%s\"}", config->key_id);

    char payload[128];
    time_t now;
    time(&now);
    snprintf(payload, sizeof(payload),
             "{\"iss\":\"%s\",\"iat\":%ld}", config->team_id, (long)now);

    /* --- Base64URL encode --- */
    char hdr_b64[128], pay_b64[256];
    size_t h_len = base64url_encode((const unsigned char *)header,
                                    strlen(header), hdr_b64, sizeof(hdr_b64));
    size_t p_len = base64url_encode((const unsigned char *)payload,
                                    strlen(payload), pay_b64, sizeof(pay_b64));
    if (h_len == 0 || p_len == 0) {
        ESP_LOGE(TAG, "Base64URL encode failed");
        goto cleanup;
    }

    /* --- Signing input: header.payload --- */
    char signing_input[512];
    snprintf(signing_input, sizeof(signing_input), "%s.%s", hdr_b64, pay_b64);

    /* SHA-256 of signing input */
    unsigned char hash[32];
    rc = mbedtls_sha256((const unsigned char *)signing_input,
                        strlen(signing_input), hash, 0);
    if (rc != 0) {
        ESP_LOGE(TAG, "SHA-256 failed");
        goto cleanup;
    }

    /* --- ECDSA sign --- */
    unsigned char sig_der[MBEDTLS_ECDSA_MAX_LEN];
    size_t sig_der_len = 0;
    rc = mbedtls_pk_sign(&pk, MBEDTLS_MD_SHA256,
                         hash, sizeof(hash),
                         sig_der, sizeof(sig_der), &sig_der_len,
                         mbedtls_ctr_drbg_random, &ctr_drbg);
    if (rc != 0) {
        ESP_LOGE(TAG, "ECDSA sign failed: -0x%04x", (unsigned)-rc);
        goto cleanup;
    }

    /* Convert DER -> raw r||s (64 bytes) */
    unsigned char sig_raw[64];
    if (der_sig_to_raw(sig_der, sig_der_len, sig_raw) != 0) {
        ESP_LOGE(TAG, "DER->raw signature conversion failed");
        goto cleanup;
    }

    /* Base64URL encode signature */
    char sig_b64[128];
    size_t s_len = base64url_encode(sig_raw, 64, sig_b64, sizeof(sig_b64));
    if (s_len == 0) {
        ESP_LOGE(TAG, "Signature base64url failed");
        goto cleanup;
    }

    /* --- Assemble JWT --- */
    int total = snprintf(jwt_buf, jwt_buf_len, "%s.%s.%s",
                         hdr_b64, pay_b64, sig_b64);
    if (total < 0 || (size_t)total >= jwt_buf_len) {
        ESP_LOGE(TAG, "JWT buffer too small");
        goto cleanup;
    }

    ESP_LOGI(TAG, "JWT generated (len=%d)", total);
    ret = ESP_OK;

cleanup:
    mbedtls_pk_free(&pk);
    mbedtls_entropy_free(&entropy);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    return ret;
}

/* ------------------------------------------------------------------ */
/*  HTTP/2 sh2lib callbacks (static context – single-threaded use)     */
/* ------------------------------------------------------------------ */

static const char *s_post_body    = NULL;
static size_t      s_post_len     = 0;
static size_t      s_post_offset  = 0;
static bool        s_resp_done    = false;
static char        s_resp_body[512];
static size_t      s_resp_len     = 0;

/**
 * Callback: nghttp2 wants POST body data from us.
 */
static int apns_send_cb(struct sh2lib_handle *handle, char *data,
                        size_t len, uint32_t *data_flags)
{
    size_t remaining = s_post_len - s_post_offset;
    size_t to_copy   = (remaining < len) ? remaining : len;

    if (to_copy > 0) {
        memcpy(data, s_post_body + s_post_offset, to_copy);
        s_post_offset += to_copy;
    }
    if (s_post_offset >= s_post_len) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    }
    return (int)to_copy;
}

/**
 * Callback: received response data from APNs.
 */
static int apns_recv_cb(struct sh2lib_handle *handle, const char *data,
                        size_t len, int flags)
{
    if (data && len > 0) {
        size_t space   = sizeof(s_resp_body) - s_resp_len - 1;
        size_t to_copy = (len < space) ? len : space;
        memcpy(s_resp_body + s_resp_len, data, to_copy);
        s_resp_len += to_copy;
        s_resp_body[s_resp_len] = '\0';
    }
    if (flags == DATA_RECV_FRAME_COMPLETE || flags == DATA_RECV_RST_STREAM) {
        s_resp_done = true;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

esp_err_t apns_send_notification(const apns_config_t *config,
                                 const apns_notification_t *notification)
{
    if (!config || !notification) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_apns_mutex, portMAX_DELAY);

    char *json_str = NULL;
    bool hd_open   = false;
    esp_err_t ret  = ESP_FAIL;

    /* ---- 1. JWT (cached; regenerate only when expired) ---- */
    time_t now;
    time(&now);
    if (s_jwt_generated_at == 0 || (now - s_jwt_generated_at) >= JWT_VALID_SECONDS) {
        ret = generate_jwt(config, s_jwt_cache, sizeof(s_jwt_cache));
        if (ret != ESP_OK) goto done;
        s_jwt_generated_at = now;
        ESP_LOGI(TAG, "JWT refreshed");
    } else {
        ESP_LOGD(TAG, "JWT cache hit (age=%lds)", (long)(now - s_jwt_generated_at));
    }

    /* ---- 2. Build JSON payload ---- */
    cJSON *root_j  = cJSON_CreateObject();
    cJSON *aps_j   = cJSON_AddObjectToObject(root_j, "aps");
    cJSON *alert_j = cJSON_AddObjectToObject(aps_j, "alert");
    cJSON_AddStringToObject(alert_j, "title", notification->title);
    cJSON_AddStringToObject(alert_j, "body",  notification->body);
    if (notification->badge >= 0) {
        cJSON_AddNumberToObject(aps_j, "badge", notification->badge);
    }
    if (notification->sound) {
        cJSON_AddStringToObject(aps_j, "sound", notification->sound);
    }
    json_str = cJSON_PrintUnformatted(root_j);
    cJSON_Delete(root_j);
    if (!json_str) {
        ESP_LOGE(TAG, "cJSON failed to build payload");
        ret = ESP_FAIL;
        goto done;
    }
    int jlen = (int)strlen(json_str);

    ESP_LOGI(TAG, "Payload (%d bytes): %s", jlen, json_str);

    /* ---- 3. Prepare static callback context ---- */
    s_post_body   = json_str;
    s_post_len    = (size_t)jlen;
    s_post_offset = 0;
    s_resp_done   = false;
    s_resp_len    = 0;
    memset(s_resp_body, 0, sizeof(s_resp_body));

    /* ---- 4. Build path & auth header ---- */
    char path[150];
    snprintf(path, sizeof(path), "/3/device/%s", notification->device_token);

    char auth_hdr[1100];
    snprintf(auth_hdr, sizeof(auth_hdr), "bearer %s", s_jwt_cache);

    const char *host = config->use_sandbox ? APNS_HOST_SANDBOX
                                           : APNS_HOST_PRODUCTION;

    /* ---- 5. Connect HTTP/2 ---- */
    char uri[128];
    snprintf(uri, sizeof(uri), "https://%s", host);

    struct sh2lib_handle hd;
    tls_keep_alive_cfg_t ka = {
        .keep_alive_enable   = true,
        .keep_alive_idle     = 5,
        .keep_alive_interval = 5,
        .keep_alive_count    = 3,
    };
    struct sh2lib_config_t sh2cfg = {
        .uri               = uri,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_cfg    = &ka,
    };

    ESP_LOGI(TAG, "Connecting to %s ...", host);
    if (sh2lib_connect(&sh2cfg, &hd) != 0) {
        ESP_LOGE(TAG, "HTTP/2 connection failed");
        goto done;
    }
    hd_open = true;
    ESP_LOGI(TAG, "HTTP/2 connected");

    /* ---- 6. Submit POST with custom headers ---- */
    const nghttp2_nv nva[] = {
        SH2LIB_MAKE_NV(":method",       "POST"),
        SH2LIB_MAKE_NV(":scheme",       "https"),
        SH2LIB_MAKE_NV(":path",         path),
        SH2LIB_MAKE_NV("host",          host),
        SH2LIB_MAKE_NV("authorization", auth_hdr),
        SH2LIB_MAKE_NV("apns-topic",    config->bundle_id),
        SH2LIB_MAKE_NV("apns-push-type","alert"),
        SH2LIB_MAKE_NV("content-type",  "application/json"),
    };

    int sid = sh2lib_do_putpost_with_nv(&hd, nva,
                                        sizeof(nva) / sizeof(nva[0]),
                                        apns_send_cb, apns_recv_cb);
    if (sid < 0) {
        ESP_LOGE(TAG, "Failed to submit POST request");
        goto done;
    }
    ESP_LOGI(TAG, "POST submitted (stream %d)", sid);

    /* ---- 7. Execute send/receive loop ---- */
    ret = ESP_FAIL;
    int rounds = 0;
    while (!s_resp_done && rounds < 150) {
        if (sh2lib_execute(&hd) != 0) {
            ESP_LOGE(TAG, "sh2lib_execute error");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        rounds++;
    }

    /* ---- 8. Evaluate response ---- */
    if (s_resp_done && s_resp_len > 0) {
        /* APNs returns an empty body on 200 OK; a JSON body means error */
        ESP_LOGW(TAG, "APNs error response: %s", s_resp_body);
        if (strstr(s_resp_body, "Unregistered") != NULL) {
            ESP_LOGW(TAG, "APNs: device token is unregistered");
            ret = APNS_ERR_UNREGISTERED;
        } else {
            ret = ESP_FAIL;
        }
    } else if (s_resp_done && s_resp_len == 0) {
        ESP_LOGI(TAG, "APNs: 200 OK");
        ret = ESP_OK;
    } else {
        ESP_LOGE(TAG, "APNs: timed out waiting for response");
        ret = ESP_FAIL;
    }

done:
    cJSON_free(json_str);
    if (hd_open) sh2lib_free(&hd);
    xSemaphoreGive(s_apns_mutex);
    return ret;
}
