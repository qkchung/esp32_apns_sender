/*
 * token_store.h — NVS-backed push token registry
 *
 * Maintains two persistent lists (send / block), keyed by IPv4 address string.
 * Each entry maps  ip (≤15 chars)  →  APNs device token (≤99 chars).
 *
 * Storage layout:
 *   NVS namespace "tok_send"  — send list
 *   NVS namespace "tok_blk"   — block list
 *   key   = IPv4 string (e.g. "192.168.1.10"), NVS key limit = 15 chars
 *   value = APNs device token string
 *
 * Prerequisites:
 *   nvs_flash_init() must be called before token_store_init().
 */
#pragma once

#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TOKEN_MAX_ENTRIES  64
#define TOKEN_IP_LEN       16   /* "255.255.255.255\0" */
#define TOKEN_LEN         100   /* APNs device token + null */

typedef struct {
    char ip[TOKEN_IP_LEN];
    char token[TOKEN_LEN];
} token_entry_t;

/**
 * @brief Initialise token store — opens both NVS namespaces to verify access.
 *        Must be called once after nvs_flash_init().
 */
esp_err_t token_store_init(void);

/* ---- Send list ---- */

/** Add or overwrite a send-list entry (ip → token). */
esp_err_t token_store_send_set(const char *ip, const char *token);

/** Look up a token by IP in the send list. Returns ESP_ERR_NVS_NOT_FOUND if absent. */
esp_err_t token_store_send_get(const char *ip, char *tok_out, size_t len);

/** Remove an entry from the send list. */
esp_err_t token_store_send_del(const char *ip);

/** Enumerate all send-list entries into @p out (up to @p max). Sets *count on return. */
esp_err_t token_store_send_list(token_entry_t *out, size_t *count, size_t max);

/* ---- Block list ---- */

/** Add or overwrite a block-list entry (ip → token). */
esp_err_t token_store_block_set(const char *ip, const char *token);

/** Look up a token by IP in the block list. Returns ESP_ERR_NVS_NOT_FOUND if absent. */
esp_err_t token_store_block_get(const char *ip, char *tok_out, size_t len);

/** Remove an entry from the block list. */
esp_err_t token_store_block_del(const char *ip);

/** Enumerate all block-list entries into @p out (up to @p max). Sets *count on return. */
esp_err_t token_store_block_list(token_entry_t *out, size_t *count, size_t max);

/* ---- Move operations ---- */

/** Move entry for @p ip from send list → block list. Fails if IP not in send list. */
esp_err_t token_store_move_to_block(const char *ip);

/** Move entry for @p ip from block list → send list. Fails if IP not in block list. */
esp_err_t token_store_move_to_send(const char *ip);

#ifdef __cplusplus
}
#endif
