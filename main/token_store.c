/*
 * token_store.c — NVS-backed push token registry implementation
 */
#include "token_store.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "token_store";

#define NS_SEND  "tok_send"
#define NS_BLOCK "tok_blk"

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                   */
/* ------------------------------------------------------------------ */

static esp_err_t ns_set(const char *ns, const char *key, const char *value)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(ns, NVS_READWRITE, &h);
    if (ret != ESP_OK) return ret;
    ret = nvs_set_str(h, key, value);
    if (ret == ESP_OK) ret = nvs_commit(h);
    nvs_close(h);
    return ret;
}

static esp_err_t ns_get(const char *ns, const char *key, char *out, size_t len)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(ns, NVS_READONLY, &h);
    if (ret != ESP_OK) return ret;
    ret = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    return ret;
}

static esp_err_t ns_del(const char *ns, const char *key)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(ns, NVS_READWRITE, &h);
    if (ret != ESP_OK) return ret;
    ret = nvs_erase_key(h, key);
    if (ret == ESP_OK) ret = nvs_commit(h);
    nvs_close(h);
    return ret;
}

static esp_err_t ns_list(const char *ns, token_entry_t *out,
                          size_t *count, size_t max)
{
    *count = 0;

    nvs_iterator_t it = NULL;
    esp_err_t ret = nvs_entry_find(NVS_DEFAULT_PART_NAME, ns,
                                   NVS_TYPE_STR, &it);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK; /* namespace is empty */
    }
    if (ret != ESP_OK) return ret;

    nvs_handle_t h;
    ret = nvs_open(ns, NVS_READONLY, &h);
    if (ret != ESP_OK) {
        nvs_release_iterator(it);
        return ret;
    }

    while (it != NULL && *count < max) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);

        strncpy(out[*count].ip, info.key, TOKEN_IP_LEN - 1);
        out[*count].ip[TOKEN_IP_LEN - 1] = '\0';

        size_t tok_len = TOKEN_LEN;
        if (nvs_get_str(h, info.key, out[*count].token, &tok_len) == ESP_OK) {
            (*count)++;
        }

        ret = nvs_entry_next(&it);
        if (ret != ESP_OK) break; /* ESP_ERR_NVS_NOT_FOUND = end, sets it=NULL */
    }

    nvs_release_iterator(it);
    nvs_close(h);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

esp_err_t token_store_init(void)
{
    nvs_handle_t h;
    esp_err_t ret;

    ret = nvs_open(NS_SEND, NVS_READWRITE, &h);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Cannot open send namespace: %d", ret);
        return ret;
    }
    nvs_close(h);

    ret = nvs_open(NS_BLOCK, NVS_READWRITE, &h);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Cannot open block namespace: %d", ret);
        return ret;
    }
    nvs_close(h);

    ESP_LOGI(TAG, "Token store initialised");
    return ESP_OK;
}

/* Send list */
esp_err_t token_store_send_set(const char *ip, const char *token)
{
    return ns_set(NS_SEND, ip, token);
}

esp_err_t token_store_send_get(const char *ip, char *tok_out, size_t len)
{
    return ns_get(NS_SEND, ip, tok_out, len);
}

esp_err_t token_store_send_del(const char *ip)
{
    return ns_del(NS_SEND, ip);
}

esp_err_t token_store_send_list(token_entry_t *out, size_t *count, size_t max)
{
    return ns_list(NS_SEND, out, count, max);
}

/* Block list */
esp_err_t token_store_block_set(const char *ip, const char *token)
{
    return ns_set(NS_BLOCK, ip, token);
}

esp_err_t token_store_block_get(const char *ip, char *tok_out, size_t len)
{
    return ns_get(NS_BLOCK, ip, tok_out, len);
}

esp_err_t token_store_block_del(const char *ip)
{
    return ns_del(NS_BLOCK, ip);
}

esp_err_t token_store_block_list(token_entry_t *out, size_t *count, size_t max)
{
    return ns_list(NS_BLOCK, out, count, max);
}

/* Move operations */
esp_err_t token_store_move_to_block(const char *ip)
{
    char tok[TOKEN_LEN];
    esp_err_t ret = token_store_send_get(ip, tok, sizeof(tok));
    if (ret != ESP_OK) return ret;
    ret = token_store_block_set(ip, tok);
    if (ret != ESP_OK) return ret;
    return token_store_send_del(ip);
}

esp_err_t token_store_move_to_send(const char *ip)
{
    char tok[TOKEN_LEN];
    esp_err_t ret = token_store_block_get(ip, tok, sizeof(tok));
    if (ret != ESP_OK) return ret;
    ret = token_store_send_set(ip, tok);
    if (ret != ESP_OK) return ret;
    return token_store_block_del(ip);
}
