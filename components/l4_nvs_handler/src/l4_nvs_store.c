/**
 * @file l4_nvs_store.c
 * @brief Low-level reusable NVS store layer implementation.
 *
 * See l4_nvs_store.h for the full API contract and design rationale.
 */

#include "l4_nvs_store.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include "sdkconfig.h"

#if CONFIG_NVS_ENCRYPTION
#include "esp_partition.h"
#endif

static const char *TAG = "l4_nvs_store";

/* =========================================================================
 * Partition labels (Kconfig-configurable, defaults: fctry / cfg / db1 / log)
 * ========================================================================= */

static const char *const s_part_labels[4] = {
    [L4_NVS_HANDLER_PART_FACTORY] = CONFIG_L4_NVS_STORE_PART_LABEL_FACTORY,
    [L4_NVS_HANDLER_PART_CFG]     = CONFIG_L4_NVS_STORE_PART_LABEL_CFG,
    [L4_NVS_HANDLER_PART_DB]      = CONFIG_L4_NVS_STORE_PART_LABEL_DB,
    [L4_NVS_HANDLER_PART_LOG]     = CONFIG_L4_NVS_STORE_PART_LABEL_LOG,
};

/* =========================================================================
 * Mutex
 * ========================================================================= */

static SemaphoreHandle_t s_mutex = NULL;

static void ensure_mutex(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
    }
}

static bool store_lock(void)
{
    ensure_mutex();
    if (s_mutex == NULL) {
        return false;
    }
    return xSemaphoreTake(s_mutex, pdMS_TO_TICKS(5000)) == pdTRUE;
}

static void store_unlock(void)
{
    if (s_mutex != NULL) {
        xSemaphoreGive(s_mutex);
    }
}

/* =========================================================================
 * Internal: partition init
 * ========================================================================= */

/**
 * Initialise a single NVS flash partition by label.
 *
 * Under CONFIG_NVS_ENCRYPTION, attempts secure init with the key partition
 * found by subtype ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS.  Falls back to plain
 * init when no suitable key partition is found.
 *
 * If the partition has no free pages or has an incompatible NVS version, it
 * is erased and re-initialised once.
 */
static esp_err_t init_partition_by_label(const char *label)
{
    esp_err_t err;

#if CONFIG_NVS_ENCRYPTION
    const esp_partition_t *keys_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS, NULL);
    if (keys_part != NULL) {
        nvs_sec_cfg_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        err = nvs_flash_read_security_cfg(keys_part, &cfg);
        if (err == ESP_OK) {
            err = nvs_flash_secure_init_partition(label, &cfg);
            if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
                err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
                ESP_LOGW(TAG, "erasing partition '%s' (err 0x%x)", label, err);
                nvs_flash_erase_partition(label);
                err = nvs_flash_secure_init_partition(label, &cfg);
            }
            if (err == ESP_OK) {
                return ESP_OK;
            }
        }
        ESP_LOGW(TAG, "secure init for '%s' failed (0x%x), trying plain init",
                 label, err);
    }
#endif /* CONFIG_NVS_ENCRYPTION */

    err = nvs_flash_init_partition(label);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "erasing partition '%s' (err 0x%x)", label, err);
        nvs_flash_erase_partition(label);
        err = nvs_flash_init_partition(label);
    }
    return err;
}

/* =========================================================================
 * Internal: open namespace with lazy partition init
 * ========================================================================= */

/**
 * Open an NVS namespace, performing lazy partition init when the partition
 * has not been initialised yet (ESP_ERR_NVS_NOT_INITIALIZED).
 *
 * Must be called while the store mutex is held.
 */
static esp_err_t open_namespace_locked(l4_nvs_handler_partition_t partition,
                                        const char *ns,
                                        nvs_open_mode_t mode,
                                        nvs_handle_t *handle)
{
    const char *label = s_part_labels[partition];
    esp_err_t err = nvs_open_from_partition(label, ns, mode, handle);

    if (err == ESP_ERR_NVS_NOT_INITIALIZED) {
        ESP_LOGI(TAG, "lazy init partition '%s'", label);
        err = init_partition_by_label(label);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "lazy init of '%s' failed: 0x%x", label, err);
            return err;
        }
        err = nvs_open_from_partition(label, ns, mode, handle);
    }

    return err;
}

/* =========================================================================
 * Partition label helpers
 * ========================================================================= */

const char *l4_nvs_store_partition_label(l4_nvs_handler_partition_t partition)
{
    if ((unsigned)partition >= 4u) {
        return NULL;
    }
    return s_part_labels[partition];
}

esp_err_t l4_nvs_store_partition_from_str(const char *str,
                                           l4_nvs_handler_partition_t *out_partition)
{
    if (str == NULL || out_partition == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Well-known canonical aliases */
    if (strcmp(str, "factory") == 0 || strcmp(str, "fctry") == 0) {
        *out_partition = L4_NVS_HANDLER_PART_FACTORY;
        return ESP_OK;
    }
    if (strcmp(str, "cfg") == 0) {
        *out_partition = L4_NVS_HANDLER_PART_CFG;
        return ESP_OK;
    }
    if (strcmp(str, "db") == 0 || strcmp(str, "db1") == 0) {
        *out_partition = L4_NVS_HANDLER_PART_DB;
        return ESP_OK;
    }
    if (strcmp(str, "log") == 0) {
        *out_partition = L4_NVS_HANDLER_PART_LOG;
        return ESP_OK;
    }

    /* Fall back: check against Kconfig-derived labels */
    for (unsigned i = 0; i < 4u; i++) {
        if (strcmp(str, s_part_labels[i]) == 0) {
            *out_partition = (l4_nvs_handler_partition_t)i;
            return ESP_OK;
        }
    }

    return ESP_ERR_NOT_FOUND;
}

/* =========================================================================
 * Partition init
 * ========================================================================= */

esp_err_t l4_nvs_store_init_partition(l4_nvs_handler_partition_t partition)
{
    if ((unsigned)partition >= 4u) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!store_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = init_partition_by_label(s_part_labels[partition]);
    store_unlock();
    return err;
}

/* =========================================================================
 * Typed reads
 * ========================================================================= */

esp_err_t l4_nvs_store_read_u8(l4_nvs_handler_partition_t partition,
                                const char *ns,
                                const char *key,
                                uint8_t *out)
{
    if (ns == NULL || key == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!store_lock()) {
        return ESP_ERR_TIMEOUT;
    }

    nvs_handle_t h;
    esp_err_t err = open_namespace_locked(partition, ns, NVS_READONLY, &h);
    if (err == ESP_OK) {
        err = nvs_get_u8(h, key, out);
        nvs_close(h);
    }
    store_unlock();
    return err;
}

esp_err_t l4_nvs_store_read_i32(l4_nvs_handler_partition_t partition,
                                 const char *ns,
                                 const char *key,
                                 int32_t *out)
{
    if (ns == NULL || key == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!store_lock()) {
        return ESP_ERR_TIMEOUT;
    }

    nvs_handle_t h;
    esp_err_t err = open_namespace_locked(partition, ns, NVS_READONLY, &h);
    if (err == ESP_OK) {
        err = nvs_get_i32(h, key, out);
        nvs_close(h);
    }
    store_unlock();
    return err;
}

esp_err_t l4_nvs_store_read_u32(l4_nvs_handler_partition_t partition,
                                 const char *ns,
                                 const char *key,
                                 uint32_t *out)
{
    if (ns == NULL || key == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!store_lock()) {
        return ESP_ERR_TIMEOUT;
    }

    nvs_handle_t h;
    esp_err_t err = open_namespace_locked(partition, ns, NVS_READONLY, &h);
    if (err == ESP_OK) {
        err = nvs_get_u32(h, key, out);
        nvs_close(h);
    }
    store_unlock();
    return err;
}

esp_err_t l4_nvs_store_read_str(l4_nvs_handler_partition_t partition,
                                 const char *ns,
                                 const char *key,
                                 char *buf,
                                 size_t buf_len)
{
    if (ns == NULL || key == NULL || buf == NULL || buf_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!store_lock()) {
        return ESP_ERR_TIMEOUT;
    }

    nvs_handle_t h;
    esp_err_t err = open_namespace_locked(partition, ns, NVS_READONLY, &h);
    if (err == ESP_OK) {
        size_t required = buf_len;
        err = nvs_get_str(h, key, buf, &required);
        nvs_close(h);
    }
    store_unlock();
    return err;
}

/* =========================================================================
 * Typed writes
 * ========================================================================= */

/**
 * Write a numeric value to an already-open NVS handle.
 *
 * Dispatches to the correct nvs_set_* variant based on @p type_str.
 * The @p value is cast to the target width before writing.
 */
static esp_err_t write_number_to_handle(nvs_handle_t h,
                                         const char *type_str,
                                         const char *key,
                                         int32_t value)
{
    if (strcmp(type_str, "i8") == 0) {
        return nvs_set_i8(h, key, (int8_t)value);
    }
    if (strcmp(type_str, "u8") == 0) {
        return nvs_set_u8(h, key, (uint8_t)value);
    }
    if (strcmp(type_str, "i16") == 0) {
        return nvs_set_i16(h, key, (int16_t)value);
    }
    if (strcmp(type_str, "u16") == 0) {
        return nvs_set_u16(h, key, (uint16_t)value);
    }
    if (strcmp(type_str, "i32") == 0) {
        return nvs_set_i32(h, key, (int32_t)value);
    }
    if (strcmp(type_str, "u32") == 0) {
        return nvs_set_u32(h, key, (uint32_t)value);
    }
    return ESP_ERR_INVALID_ARG;
}

esp_err_t l4_nvs_store_set_str(l4_nvs_handler_partition_t partition,
                                const char *ns,
                                const char *key,
                                const char *value)
{
    if (ns == NULL || key == NULL || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!store_lock()) {
        return ESP_ERR_TIMEOUT;
    }

    nvs_handle_t h;
    esp_err_t err = open_namespace_locked(partition, ns, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, key, value);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    store_unlock();
    return err;
}

esp_err_t l4_nvs_store_set_number(l4_nvs_handler_partition_t partition,
                                   const char *ns,
                                   const char *type_str,
                                   const char *key,
                                   int32_t value)
{
    if (ns == NULL || type_str == NULL || key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!store_lock()) {
        return ESP_ERR_TIMEOUT;
    }

    nvs_handle_t h;
    esp_err_t err = open_namespace_locked(partition, ns, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = write_number_to_handle(h, type_str, key, value);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    store_unlock();
    return err;
}

/* =========================================================================
 * DB partition convenience helpers
 * ========================================================================= */

esp_err_t l4_nvs_store_get_str_from_db(const char *ns,
                                        const char *key,
                                        char *buf,
                                        size_t buf_len)
{
    return l4_nvs_store_read_str(L4_NVS_HANDLER_PART_DB, ns, key, buf, buf_len);
}

esp_err_t l4_nvs_store_get_u32_from_db(const char *ns,
                                        const char *key,
                                        uint32_t *out)
{
    return l4_nvs_store_read_u32(L4_NVS_HANDLER_PART_DB, ns, key, out);
}

/* =========================================================================
 * Key erase
 * ========================================================================= */

esp_err_t l4_nvs_store_erase_key(l4_nvs_handler_partition_t partition,
                                  const char *ns,
                                  const char *key)
{
    if (ns == NULL || key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!store_lock()) {
        return ESP_ERR_TIMEOUT;
    }

    nvs_handle_t h;
    esp_err_t err = open_namespace_locked(partition, ns, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_erase_key(h, key);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    store_unlock();
    return err;
}

/* =========================================================================
 * JSON namespace dump
 * ========================================================================= */

/**
 * Map an NVS type to the type string used in the JSON output.
 * Returns NULL for unsupported types (blob, u64, i64) which are skipped.
 */
static const char *nvs_type_to_str(nvs_type_t type)
{
    switch (type) {
    case NVS_TYPE_I8:  return "i8";
    case NVS_TYPE_U8:  return "u8";
    case NVS_TYPE_I16: return "i16";
    case NVS_TYPE_U16: return "u16";
    case NVS_TYPE_I32: return "i32";
    case NVS_TYPE_U32: return "u32";
    case NVS_TYPE_STR: return "str";
    default:           return NULL;
    }
}

/**
 * Append one entry to @p arr from an open NVS handle.
 *
 * Returns true if the entry was successfully appended, false on error or if
 * the type is unsupported (entry is silently skipped in that case).
 */
static bool append_entry(cJSON *arr,
                          nvs_handle_t h,
                          const nvs_entry_info_t *info)
{
    const char *type_str = nvs_type_to_str(info->type);
    if (type_str == NULL) {
        return false; /* unsupported type: skip silently */
    }

    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) {
        return false;
    }
    cJSON_AddStringToObject(obj, "k", info->key);
    cJSON_AddStringToObject(obj, "t", type_str);

    bool value_added = false;

    if (info->type == NVS_TYPE_STR) {
        size_t str_len = 0;
        if (nvs_get_str(h, info->key, NULL, &str_len) == ESP_OK && str_len > 0) {
            char *sbuf = malloc(str_len);
            if (sbuf != NULL) {
                if (nvs_get_str(h, info->key, sbuf, &str_len) == ESP_OK) {
                    cJSON_AddStringToObject(obj, "v", sbuf);
                    value_added = true;
                }
                free(sbuf);
            }
        }
    } else {
        /* Numeric type: read and add as JSON number */
        double num = 0.0;
        esp_err_t read_err = ESP_FAIL;

        switch (info->type) {
        case NVS_TYPE_I8:  { int8_t   v = 0; read_err = nvs_get_i8(h,  info->key, &v); num = (double)v; } break;
        case NVS_TYPE_U8:  { uint8_t  v = 0; read_err = nvs_get_u8(h,  info->key, &v); num = (double)v; } break;
        case NVS_TYPE_I16: { int16_t  v = 0; read_err = nvs_get_i16(h, info->key, &v); num = (double)v; } break;
        case NVS_TYPE_U16: { uint16_t v = 0; read_err = nvs_get_u16(h, info->key, &v); num = (double)v; } break;
        case NVS_TYPE_I32: { int32_t  v = 0; read_err = nvs_get_i32(h, info->key, &v); num = (double)v; } break;
        case NVS_TYPE_U32: { uint32_t v = 0; read_err = nvs_get_u32(h, info->key, &v); num = (double)v; } break;
        default: break;
        }
        if (read_err == ESP_OK) {
            cJSON_AddNumberToObject(obj, "v", num);
            value_added = true;
        }
    }

    if (value_added) {
        cJSON_AddItemToArray(arr, obj);
        return true;
    }

    cJSON_Delete(obj);
    return false;
}

esp_err_t l4_nvs_store_get_json(l4_nvs_handler_partition_t partition,
                                 const char *ns,
                                 char **out_json)
{
    if (ns == NULL || out_json == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_json = NULL;

    if (!store_lock()) {
        return ESP_ERR_TIMEOUT;
    }

    const char *label = s_part_labels[partition];

    /* Verify namespace exists (lazy partition init on NOT_INITIALIZED) */
    nvs_handle_t h;
    esp_err_t err = open_namespace_locked(partition, ns, NVS_READONLY, &h);
    if (err != ESP_OK) {
        store_unlock();
        return err; /* ESP_ERR_NVS_NOT_FOUND if namespace absent */
    }

    cJSON *arr = cJSON_CreateArray();
    if (arr == NULL) {
        nvs_close(h);
        store_unlock();
        return ESP_ERR_NO_MEM;
    }

    /* Iterate entries; use the already-open handle to read values */
    nvs_iterator_t it = NULL;
    esp_err_t it_err = nvs_entry_find(label, ns, NVS_TYPE_ANY, &it);

    int count = 0;
    while (it_err == ESP_OK && it != NULL) {
        if (count >= CONFIG_L4_NVS_STORE_MAX_ENTRIES) {
            ESP_LOGW(TAG, "max entries (%d) reached in ns='%s'",
                     CONFIG_L4_NVS_STORE_MAX_ENTRIES, ns);
            nvs_release_iterator(it);
            it = NULL;
            break;
        }

        nvs_entry_info_t info;
        nvs_entry_info(it, &info);

        if (append_entry(arr, h, &info)) {
            count++;
        }

        it_err = nvs_entry_next(&it);
    }

    /* nvs_entry_next sets *it to NULL and releases on ESP_ERR_NVS_NOT_FOUND.
     * Release manually only if we broke out early. */
    if (it != NULL) {
        nvs_release_iterator(it);
    }

    nvs_close(h);

    /* ESP_ERR_NVS_NOT_FOUND from iterator just means end-of-entries */
    if (it_err != ESP_OK && it_err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "iterator error 0x%x ns='%s'", it_err, ns);
        cJSON_Delete(arr);
        store_unlock();
        return it_err;
    }

    char *json_str = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    store_unlock();

    if (json_str == NULL) {
        return ESP_ERR_NO_MEM;
    }
    *out_json = json_str;
    return ESP_OK;
}

/* =========================================================================
 * Backend factory for l4_nvs_handler
 * ========================================================================= */

static esp_err_t store_backend_partition_from_str(
    const char *str,
    l4_nvs_handler_partition_t *out_partition,
    void *ctx)
{
    (void)ctx;
    return l4_nvs_store_partition_from_str(str, out_partition);
}

static esp_err_t store_backend_set_str(
    l4_nvs_handler_partition_t partition,
    const char *ns,
    const char *key,
    const char *value,
    void *ctx)
{
    (void)ctx;
    return l4_nvs_store_set_str(partition, ns, key, value);
}

static esp_err_t store_backend_set_number(
    l4_nvs_handler_partition_t partition,
    const char *ns,
    const char *type_str,
    const char *key,
    int32_t value,
    void *ctx)
{
    (void)ctx;
    return l4_nvs_store_set_number(partition, ns, type_str, key, value);
}

static esp_err_t store_backend_erase_key(
    l4_nvs_handler_partition_t partition,
    const char *ns,
    const char *key,
    void *ctx)
{
    (void)ctx;
    return l4_nvs_store_erase_key(partition, ns, key);
}

static esp_err_t store_backend_get_json(
    l4_nvs_handler_partition_t partition,
    const char *ns,
    char **out_json,
    void *ctx)
{
    (void)ctx;
    return l4_nvs_store_get_json(partition, ns, out_json);
}

l4_nvs_handler_backend_t l4_nvs_handler_make_store_backend(void)
{
    l4_nvs_handler_backend_t backend = {
        .partition_from_str = store_backend_partition_from_str,
        .set_str            = store_backend_set_str,
        .set_number         = store_backend_set_number,
        .erase_key          = store_backend_erase_key,
        .get_json           = store_backend_get_json,
        .ctx                = NULL,
    };
    return backend;
}

esp_err_t l4_nvs_handler_init_with_store_backend(void)
{
    l4_nvs_handler_backend_t backend = l4_nvs_handler_make_store_backend();
    return l4_nvs_handler_init(&backend);
}
