/*
 * nvs_store.c - see nvs_store.h. Namespace "safq": records as blobs
 * under "r%08x", the sequence ceiling as the u32 "ceil".
 */
#include "nvs_store.h"

#include <stdio.h>

#include "esp_check.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *NS = "safq";
static nvs_handle_t s_nvs;

static void key_for(uint32_t idx, char out[16])
{
    snprintf(out, 16, "r%08x", (unsigned)idx);
}

static bool ns_put(void *ctx, uint32_t idx, const void *rec, size_t len)
{
    (void)ctx;
    char key[16];
    key_for(idx, key);
    return nvs_set_blob(s_nvs, key, rec, len) == ESP_OK &&
           nvs_commit(s_nvs) == ESP_OK;
}

static bool ns_get(void *ctx, uint32_t idx, void *rec, size_t *len)
{
    (void)ctx;
    char key[16];
    key_for(idx, key);
    return nvs_get_blob(s_nvs, key, rec, len) == ESP_OK;
}

static bool ns_erase(void *ctx, uint32_t idx)
{
    (void)ctx;
    char key[16];
    key_for(idx, key);
    return nvs_erase_key(s_nvs, key) == ESP_OK &&
           nvs_commit(s_nvs) == ESP_OK;
}

static void ns_scan(void *ctx, uint32_t *first, uint32_t *last, bool *any)
{
    (void)ctx;
    *any = false;
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, NS,
                                   NVS_TYPE_BLOB, &it);
    while (err == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        unsigned idx;
        if (sscanf(info.key, "r%8x", &idx) == 1) {
            if (!*any || idx < *first)
                *first = idx;
            if (!*any || idx > *last)
                *last = idx;
            *any = true;
        }
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
}

static bool ns_get_meta(void *ctx, uint32_t *v)
{
    (void)ctx;
    return nvs_get_u32(s_nvs, "ceil", v) == ESP_OK;
}

static bool ns_put_meta(void *ctx, uint32_t v)
{
    (void)ctx;
    return nvs_set_u32(s_nvs, "ceil", v) == ESP_OK &&
           nvs_commit(s_nvs) == ESP_OK;
}

void nvs_store_init(saf_ops *ops)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(nvs_open(NS, NVS_READWRITE, &s_nvs));

    ops->store_put = ns_put;
    ops->store_get = ns_get;
    ops->store_erase = ns_erase;
    ops->store_scan = ns_scan;
    ops->store_get_meta = ns_get_meta;
    ops->store_put_meta = ns_put_meta;
}
