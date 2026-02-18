/*
 * token_store.c — NVS-backed push token registry implementation
 */
#include "token_store.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "token_store";

#define NS_SEND_S  "tok_snd_s"   /* sandbox send      */
#define NS_SEND_P  "tok_snd_p"   /* production send   */
#define NS_BLOCK_S "tok_blk_s"   /* sandbox block     */
#define NS_BLOCK_P "tok_blk_p"   /* production block  */

static const char *send_ns(const char *server_type)
{
    return (strcmp(server_type, "production") == 0) ? NS_SEND_P : NS_SEND_S;
}

static const char *block_ns(const char *server_type)
{
    return (strcmp(server_type, "production") == 0) ? NS_BLOCK_P : NS_BLOCK_S;
}

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

/* Like ns_list but also populates the server_type field of each entry. */
static esp_err_t ns_list_tagged(const char *ns, const char *server_type,
                                 token_entry_t *out, size_t *count, size_t max)
{
    *count = 0;

    nvs_iterator_t it = NULL;
    esp_err_t ret = nvs_entry_find(NVS_DEFAULT_PART_NAME, ns,
                                   NVS_TYPE_STR, &it);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
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

        strncpy(out[*count].server_type, server_type, TOKEN_SERVER_TYPE_LEN - 1);
        out[*count].server_type[TOKEN_SERVER_TYPE_LEN - 1] = '\0';

        size_t tok_len = TOKEN_LEN;
        if (nvs_get_str(h, info.key, out[*count].token, &tok_len) == ESP_OK) {
            (*count)++;
        }

        ret = nvs_entry_next(&it);
        if (ret != ESP_OK) break;
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

    const char *namespaces[] = { NS_SEND_S, NS_SEND_P, NS_BLOCK_S, NS_BLOCK_P };
    for (int i = 0; i < 4; i++) {
        ret = nvs_open(namespaces[i], NVS_READWRITE, &h);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Cannot open namespace %s: %d", namespaces[i], ret);
            return ret;
        }
        nvs_close(h);
    }

    ESP_LOGI(TAG, "Token store initialised");
    return ESP_OK;
}

/* Send list */
esp_err_t token_store_send_set(const char *server_type, const char *ip, const char *token)
{
    return ns_set(send_ns(server_type), ip, token);
}

esp_err_t token_store_send_get(const char *server_type, const char *ip, char *tok_out, size_t len)
{
    return ns_get(send_ns(server_type), ip, tok_out, len);
}

esp_err_t token_store_send_del(const char *ip)
{
    esp_err_t r1 = ns_del(NS_SEND_S, ip);
    esp_err_t r2 = ns_del(NS_SEND_P, ip);
    if (r1 == ESP_OK || r2 == ESP_OK) return ESP_OK;
    if (r1 == ESP_ERR_NVS_NOT_FOUND && r2 == ESP_ERR_NVS_NOT_FOUND)
        return ESP_ERR_NVS_NOT_FOUND;
    return (r1 != ESP_OK) ? r1 : r2;
}

esp_err_t token_store_send_list(token_entry_t *out, size_t *count, size_t max)
{
    size_t c1 = 0;
    ns_list_tagged(NS_SEND_S, "sandbox", out, &c1, max);
    size_t c2 = 0;
    if (c1 < max)
        ns_list_tagged(NS_SEND_P, "production", out + c1, &c2, max - c1);
    *count = c1 + c2;
    return ESP_OK;
}

esp_err_t token_store_send_list_type(const char *server_type, token_entry_t *out,
                                      size_t *count, size_t max)
{
    return ns_list_tagged(send_ns(server_type), server_type, out, count, max);
}

/* Block list */
esp_err_t token_store_block_set(const char *ip, const char *token)
{
    esp_err_t r1 = ns_set(NS_BLOCK_S, ip, token);
    esp_err_t r2 = ns_set(NS_BLOCK_P, ip, token);
    return (r1 == ESP_OK && r2 == ESP_OK) ? ESP_OK : (r1 != ESP_OK ? r1 : r2);
}

esp_err_t token_store_block_get(const char *server_type, const char *ip, char *tok_out, size_t len)
{
    return ns_get(block_ns(server_type), ip, tok_out, len);
}

esp_err_t token_store_block_del(const char *ip)
{
    esp_err_t r1 = ns_del(NS_BLOCK_S, ip);
    esp_err_t r2 = ns_del(NS_BLOCK_P, ip);
    if (r1 == ESP_OK || r2 == ESP_OK) return ESP_OK;
    if (r1 == ESP_ERR_NVS_NOT_FOUND && r2 == ESP_ERR_NVS_NOT_FOUND)
        return ESP_ERR_NVS_NOT_FOUND;
    return (r1 != ESP_OK) ? r1 : r2;
}

esp_err_t token_store_block_list(token_entry_t *out, size_t *count, size_t max)
{
    size_t c1 = 0;
    ns_list_tagged(NS_BLOCK_S, "sandbox", out, &c1, max);
    size_t c2 = 0;
    if (c1 < max)
        ns_list_tagged(NS_BLOCK_P, "production", out + c1, &c2, max - c1);
    *count = c1 + c2;
    return ESP_OK;
}

/* Move operations — apply to both server types */
esp_err_t token_store_move_to_block(const char *ip)
{
    char tok[TOKEN_LEN];
    bool moved = false;

    if (ns_get(NS_SEND_S, ip, tok, sizeof(tok)) == ESP_OK) {
        ns_set(NS_BLOCK_S, ip, tok);
        ns_del(NS_SEND_S, ip);
        moved = true;
    }
    if (ns_get(NS_SEND_P, ip, tok, sizeof(tok)) == ESP_OK) {
        ns_set(NS_BLOCK_P, ip, tok);
        ns_del(NS_SEND_P, ip);
        moved = true;
    }
    return moved ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t token_store_move_to_send(const char *ip)
{
    char tok[TOKEN_LEN];
    bool moved = false;

    if (ns_get(NS_BLOCK_S, ip, tok, sizeof(tok)) == ESP_OK) {
        ns_set(NS_SEND_S, ip, tok);
        ns_del(NS_BLOCK_S, ip);
        moved = true;
    }
    if (ns_get(NS_BLOCK_P, ip, tok, sizeof(tok)) == ESP_OK) {
        ns_set(NS_SEND_P, ip, tok);
        ns_del(NS_BLOCK_P, ip);
        moved = true;
    }
    return moved ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}
