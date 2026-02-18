/*
 * token_store.h — NVS-backed push token registry
 *
 * Maintains two persistent lists (send / block), keyed by IPv4 address string.
 * Each entry maps  (server_type, ip)  →  APNs device token.
 *
 * Storage layout (NVS key limit = 15 chars, so separate namespaces per type):
 *   NVS namespace "tok_snd_s"  — sandbox send list
 *   NVS namespace "tok_snd_p"  — production send list
 *   NVS namespace "tok_blk_s"  — sandbox block list
 *   NVS namespace "tok_blk_p"  — production block list
 *   key   = IPv4 string (e.g. "192.168.1.10")
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

#define TOKEN_MAX_ENTRIES      64
#define TOKEN_IP_LEN           16   /* "255.255.255.255\0" */
#define TOKEN_LEN             100   /* APNs device token + null */
#define TOKEN_SERVER_TYPE_LEN  12   /* "sandbox\0" or "production\0" */

typedef struct {
    char ip[TOKEN_IP_LEN];
    char token[TOKEN_LEN];
    char server_type[TOKEN_SERVER_TYPE_LEN];   /* "sandbox" or "production" */
} token_entry_t;

/**
 * @brief Initialise token store — opens both NVS namespaces to verify access.
 *        Must be called once after nvs_flash_init().
 */
esp_err_t token_store_init(void);

/* ---- Send list ---- */

/** Add or overwrite a send-list entry for the given server_type ("sandbox" or "production"). */
esp_err_t token_store_send_set(const char *server_type, const char *ip, const char *token);

/** Look up a token by server_type + IP in the send list. Returns ESP_ERR_NVS_NOT_FOUND if absent. */
esp_err_t token_store_send_get(const char *server_type, const char *ip, char *tok_out, size_t len);

/** Remove an entry from the send list for @p ip — applies to both sandbox and production. */
esp_err_t token_store_send_del(const char *ip);

/** Enumerate all send-list entries (both server types) into @p out. server_type field is populated. */
esp_err_t token_store_send_list(token_entry_t *out, size_t *count, size_t max);

/** Enumerate send-list entries for a specific server_type only. Used by /blast. */
esp_err_t token_store_send_list_type(const char *server_type, token_entry_t *out, size_t *count, size_t max);

/* ---- Block list ---- */

/** Add or overwrite a block-list entry for @p ip — applies to both sandbox and production. */
esp_err_t token_store_block_set(const char *ip, const char *token);

/** Look up a token by server_type + IP in the block list. Returns ESP_ERR_NVS_NOT_FOUND if absent. */
esp_err_t token_store_block_get(const char *server_type, const char *ip, char *tok_out, size_t len);

/** Remove an entry from the block list for @p ip — applies to both sandbox and production. */
esp_err_t token_store_block_del(const char *ip);

/** Enumerate all block-list entries (both server types) into @p out. server_type field is populated. */
esp_err_t token_store_block_list(token_entry_t *out, size_t *count, size_t max);

/* ---- Move operations (IP only — apply to both server types) ---- */

/** Move entry for @p ip from send list → block list. Succeeds if found in either server type. */
esp_err_t token_store_move_to_block(const char *ip);

/** Move entry for @p ip from block list → send list. Succeeds if found in either server type. */
esp_err_t token_store_move_to_send(const char *ip);

#ifdef __cplusplus
}
#endif
