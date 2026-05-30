/**
 * @file l4_nvs_handler.c
 * @brief Generic NVS record handler implementation.
 *
 * See l4_nvs_handler.h for the full API contract and design rationale.
 */

#include "l4_nvs_handler.h"

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <limits.h>

#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "l4_nvs_handler";

/* =========================================================================
 * Module state
 * ========================================================================= */

static l4_nvs_handler_backend_t s_backend;
static bool                     s_initialized = false;

/* =========================================================================
 * Namespace materializer table
 * ========================================================================= */

typedef struct {
    char                            namespace_name[L4_NVS_HANDLER_NS_MAX_LEN + 1];
    l4_nvs_handler_materialize_fn_t fn;
    void                           *ctx;
    bool                            in_use;
} materializer_entry_t;

static materializer_entry_t s_materializers[CONFIG_L4_NVS_HANDLER_MAX_MATERIALIZERS];

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/**
 * Run the materializer for @p namespace_name, if one is registered.
 * A non-ESP_OK return from the callback is logged as a warning but does not
 * abort the caller.
 */
static void run_materializer(const char *namespace_name)
{
    for (int i = 0; i < CONFIG_L4_NVS_HANDLER_MAX_MATERIALIZERS; i++) {
        if (!s_materializers[i].in_use) {
            continue;
        }
        if (strcmp(s_materializers[i].namespace_name, namespace_name) == 0) {
            esp_err_t ret = s_materializers[i].fn(s_materializers[i].ctx);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "materializer for ns '%s' returned 0x%x",
                         namespace_name, ret);
            }
            return;
        }
    }
}

/**
 * Infer a partition from a namespace name when the "p" field is absent.
 *
 * Rule:
 *   namespace starts with "cfg_"  →  L4_NVS_HANDLER_PART_CFG
 *   otherwise                     →  L4_NVS_HANDLER_PART_DB
 */
static l4_nvs_handler_partition_t infer_partition(const char *namespace_name)
{
    if (strncmp(namespace_name, "cfg_", 4) == 0) {
        return L4_NVS_HANDLER_PART_CFG;
    }
    return L4_NVS_HANDLER_PART_DB;
}

/**
 * Parse a numeric string or JSON number into a signed 32-bit integer.
 *
 * Accepts:
 *   - cJSON_IsNumber items  (direct cast, no heap allocation)
 *   - cJSON_IsString items  (strtol / strtoul parse)
 *
 * Using a local stack buffer avoids the malloc(32) leak present in the
 * original implementation's code path.
 *
 * @return true on success, false if the value cannot be parsed.
 */
static bool parse_numeric_value(const cJSON *val_json, int32_t *out_val)
{
    if (cJSON_IsNumber(val_json)) {
        /* Direct read from cJSON double — no heap allocation needed */
        *out_val = (int32_t)val_json->valuedouble;
        return true;
    }

    if (cJSON_IsString(val_json) && val_json->valuestring != NULL) {
        char *end = NULL;
        errno = 0;
        long parsed = strtol(val_json->valuestring, &end, 10);
        if (errno != 0 || end == val_json->valuestring || *end != '\0') {
            return false;
        }
        if (parsed < INT32_MIN || parsed > INT32_MAX) {
            return false;
        }
        *out_val = (int32_t)parsed;
        return true;
    }

    return false;
}

/**
 * Return true if @p type_str is one of the supported numeric types.
 */
static bool is_numeric_type(const char *type_str)
{
    return (strcmp(type_str, L4_NVS_HANDLER_TYPE_I8)  == 0 ||
            strcmp(type_str, L4_NVS_HANDLER_TYPE_U8)  == 0 ||
            strcmp(type_str, L4_NVS_HANDLER_TYPE_I16) == 0 ||
            strcmp(type_str, L4_NVS_HANDLER_TYPE_U16) == 0 ||
            strcmp(type_str, L4_NVS_HANDLER_TYPE_I32) == 0 ||
            strcmp(type_str, L4_NVS_HANDLER_TYPE_U32) == 0);
}

/**
 * Apply a single already-parsed record via the registered backend.
 *
 * Dispatches to erase_key, set_str, or set_number as appropriate.
 */
static l4_nvs_handler_error_t apply_record(const l4_nvs_handler_record_t *rec)
{
    esp_err_t err;

    if (rec->is_delete_sentinel) {
        if (s_backend.erase_key == NULL) {
            ESP_LOGE(TAG, "backend erase_key callback is NULL");
            return L4_NVS_HANDLER_ERR_NVS_ACCESS;
        }
        err = s_backend.erase_key(rec->partition,
                                   rec->namespace_name,
                                   rec->key,
                                   s_backend.ctx);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "erase_key failed (0x%x) ns='%s' k='%s'",
                     err, rec->namespace_name, rec->key);
            return L4_NVS_HANDLER_ERR_NVS_ACCESS;
        }
        return L4_NVS_HANDLER_OK;
    }

    if (rec->is_str) {
        if (s_backend.set_str == NULL) {
            ESP_LOGE(TAG, "backend set_str callback is NULL");
            return L4_NVS_HANDLER_ERR_NVS_ACCESS;
        }
        err = s_backend.set_str(rec->partition,
                                 rec->namespace_name,
                                 rec->key,
                                 rec->str_value,
                                 s_backend.ctx);
        if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
            return L4_NVS_HANDLER_ERR_NVS_FULL;
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "set_str failed (0x%x) ns='%s' k='%s'",
                     err, rec->namespace_name, rec->key);
            return L4_NVS_HANDLER_ERR_NVS_ACCESS;
        }
        return L4_NVS_HANDLER_OK;
    }

    /* Numeric type */
    if (s_backend.set_number == NULL) {
        ESP_LOGE(TAG, "backend set_number callback is NULL");
        return L4_NVS_HANDLER_ERR_NVS_ACCESS;
    }
    err = s_backend.set_number(rec->partition,
                                rec->namespace_name,
                                rec->type_str,
                                rec->key,
                                rec->num_value,
                                s_backend.ctx);
    if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
        return L4_NVS_HANDLER_ERR_NVS_FULL;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_number failed (0x%x) ns='%s' k='%s' t='%s'",
                 err, rec->namespace_name, rec->key, rec->type_str);
        return L4_NVS_HANDLER_ERR_NVS_ACCESS;
    }
    return L4_NVS_HANDLER_OK;
}

/* =========================================================================
 * Public API implementation
 * ========================================================================= */

esp_err_t l4_nvs_handler_register_namespace_materializer(
    const char *namespace_name,
    l4_nvs_handler_materialize_fn_t fn,
    void *ctx)
{
    if (namespace_name == NULL || fn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Overwrite an existing entry for the same namespace */
    for (int i = 0; i < CONFIG_L4_NVS_HANDLER_MAX_MATERIALIZERS; i++) {
        if (s_materializers[i].in_use &&
            strcmp(s_materializers[i].namespace_name, namespace_name) == 0) {
            s_materializers[i].fn  = fn;
            s_materializers[i].ctx = ctx;
            return ESP_OK;
        }
    }

    /* Find a free slot */
    for (int i = 0; i < CONFIG_L4_NVS_HANDLER_MAX_MATERIALIZERS; i++) {
        if (!s_materializers[i].in_use) {
            strncpy(s_materializers[i].namespace_name, namespace_name,
                    L4_NVS_HANDLER_NS_MAX_LEN);
            s_materializers[i].namespace_name[L4_NVS_HANDLER_NS_MAX_LEN] = '\0';
            s_materializers[i].fn     = fn;
            s_materializers[i].ctx    = ctx;
            s_materializers[i].in_use = true;
            return ESP_OK;
        }
    }

    ESP_LOGE(TAG, "materializer table full (max %d)", CONFIG_L4_NVS_HANDLER_MAX_MATERIALIZERS);
    return ESP_ERR_NO_MEM;
}

esp_err_t l4_nvs_handler_init(const l4_nvs_handler_backend_t *backend)
{
    if (backend == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_backend     = *backend;
    s_initialized = true;

    /* Run all registered materializers */
    for (int i = 0; i < CONFIG_L4_NVS_HANDLER_MAX_MATERIALIZERS; i++) {
        if (s_materializers[i].in_use) {
            run_materializer(s_materializers[i].namespace_name);
        }
    }

    ESP_LOGI(TAG, "initialized");
    return ESP_OK;
}

l4_nvs_handler_error_t l4_nvs_handler_validate_and_parse_record(
    const cJSON *record_json,
    l4_nvs_handler_record_t *out_record)
{
    if (record_json == NULL || out_record == NULL) {
        return L4_NVS_HANDLER_ERR_INVALID_STRUCTURE;
    }
    if (!cJSON_IsObject(record_json)) {
        return L4_NVS_HANDLER_ERR_INVALID_STRUCTURE;
    }

    memset(out_record, 0, sizeof(*out_record));

    /* ------------------------------------------------------------------ */
    /* Required: "ns"                                                       */
    /* ------------------------------------------------------------------ */
    const cJSON *ns_json = cJSON_GetObjectItemCaseSensitive(record_json, "ns");
    if (ns_json == NULL) {
        return L4_NVS_HANDLER_ERR_MISSING_NAMESPACE;
    }
    if (!cJSON_IsString(ns_json) || ns_json->valuestring == NULL ||
        ns_json->valuestring[0] == '\0') {
        return L4_NVS_HANDLER_ERR_INVALID_NAMESPACE;
    }
    size_t ns_len = strlen(ns_json->valuestring);
    if (ns_len > L4_NVS_HANDLER_NS_MAX_LEN) {
        return L4_NVS_HANDLER_ERR_INVALID_NAMESPACE;
    }
    memcpy(out_record->namespace_name, ns_json->valuestring, ns_len + 1);

    /* ------------------------------------------------------------------ */
    /* Required: "k"                                                        */
    /* ------------------------------------------------------------------ */
    const cJSON *k_json = cJSON_GetObjectItemCaseSensitive(record_json, "k");
    if (k_json == NULL) {
        return L4_NVS_HANDLER_ERR_INVALID_KEY;
    }
    if (!cJSON_IsString(k_json) || k_json->valuestring == NULL ||
        k_json->valuestring[0] == '\0') {
        return L4_NVS_HANDLER_ERR_INVALID_KEY;
    }
    size_t k_len = strlen(k_json->valuestring);
    if (k_len > L4_NVS_HANDLER_KEY_MAX_LEN) {
        return L4_NVS_HANDLER_ERR_INVALID_KEY;
    }
    memcpy(out_record->key, k_json->valuestring, k_len + 1);

    /* ------------------------------------------------------------------ */
    /* Required: "t"                                                        */
    /* ------------------------------------------------------------------ */
    const cJSON *t_json = cJSON_GetObjectItemCaseSensitive(record_json, "t");
    if (t_json == NULL) {
        return L4_NVS_HANDLER_ERR_MISSING_FIELD;
    }
    if (!cJSON_IsString(t_json) || t_json->valuestring == NULL) {
        return L4_NVS_HANDLER_ERR_INVALID_TYPE;
    }
    const char *type_str = t_json->valuestring;

    bool type_is_str     = (strcmp(type_str, L4_NVS_HANDLER_TYPE_STR) == 0);
    bool type_is_numeric = is_numeric_type(type_str);

    if (!type_is_str && !type_is_numeric) {
        return L4_NVS_HANDLER_ERR_INVALID_TYPE;
    }
    strncpy(out_record->type_str, type_str, sizeof(out_record->type_str) - 1);
    out_record->type_str[sizeof(out_record->type_str) - 1] = '\0';
    out_record->is_str = type_is_str;

    /* ------------------------------------------------------------------ */
    /* Required: "v"                                                        */
    /* ------------------------------------------------------------------ */
    const cJSON *v_json = cJSON_GetObjectItemCaseSensitive(record_json, "v");
    if (v_json == NULL) {
        return L4_NVS_HANDLER_ERR_MISSING_FIELD;
    }

    if (type_is_str) {
        if (!cJSON_IsString(v_json) || v_json->valuestring == NULL) {
            return L4_NVS_HANDLER_ERR_INVALID_VALUE;
        }
        /* Delete sentinel: t == "str" && v == "" */
        if (v_json->valuestring[0] == '\0') {
            out_record->is_delete_sentinel = true;
        }
        out_record->str_value = v_json->valuestring;  /* borrowed pointer */
    } else {
        /* Numeric type: accept JSON number or numeric string.
         * Use a local stack buffer — no heap allocation needed — to avoid
         * the malloc(32) leak present in the original implementation. */
        int32_t num = 0;
        if (!parse_numeric_value(v_json, &num)) {
            return L4_NVS_HANDLER_ERR_INVALID_VALUE;
        }
        out_record->num_value = num;
    }

    /* ------------------------------------------------------------------ */
    /* Optional: "p" (partition)                                            */
    /* ------------------------------------------------------------------ */
    const cJSON *p_json = cJSON_GetObjectItemCaseSensitive(record_json, "p");
    if (p_json != NULL) {
        if (!cJSON_IsString(p_json) || p_json->valuestring == NULL ||
            p_json->valuestring[0] == '\0') {
            return L4_NVS_HANDLER_ERR_INVALID_PARTITION;
        }
        if (s_backend.partition_from_str == NULL) {
            return L4_NVS_HANDLER_ERR_INVALID_PARTITION;
        }
        esp_err_t ret = s_backend.partition_from_str(p_json->valuestring,
                                                      &out_record->partition,
                                                      s_backend.ctx);
        if (ret != ESP_OK) {
            return L4_NVS_HANDLER_ERR_INVALID_PARTITION;
        }
    } else {
        /* Infer partition from namespace prefix */
        out_record->partition = infer_partition(out_record->namespace_name);
    }

    return L4_NVS_HANDLER_OK;
}

l4_nvs_handler_error_t l4_nvs_handler_set_records(const cJSON *records_array)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "not initialized");
        return L4_NVS_HANDLER_ERR_NVS_ACCESS;
    }
    if (records_array == NULL || !cJSON_IsArray(records_array)) {
        return L4_NVS_HANDLER_ERR_INVALID_STRUCTURE;
    }

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, records_array) {
        l4_nvs_handler_record_t rec;
        l4_nvs_handler_error_t  parse_err =
            l4_nvs_handler_validate_and_parse_record(item, &rec);
        if (parse_err != L4_NVS_HANDLER_OK) {
            ESP_LOGW(TAG, "record validation failed: %d", parse_err);
            return parse_err;
        }

        /* Run materializer before first write to the namespace */
        run_materializer(rec.namespace_name);

        l4_nvs_handler_error_t apply_err = apply_record(&rec);
        if (apply_err != L4_NVS_HANDLER_OK) {
            return apply_err;
        }
    }

    return L4_NVS_HANDLER_OK;
}

l4_nvs_handler_error_t l4_nvs_handler_get_records(
    const char *namespace_name,
    char **out_json_array)
{
    if (out_json_array == NULL) {
        return L4_NVS_HANDLER_ERR_UNKNOWN;
    }
    *out_json_array = NULL;

    if (!s_initialized) {
        ESP_LOGE(TAG, "not initialized");
        return L4_NVS_HANDLER_ERR_NVS_ACCESS;
    }
    if (namespace_name == NULL || namespace_name[0] == '\0') {
        return L4_NVS_HANDLER_ERR_MISSING_NAMESPACE;
    }
    if (strlen(namespace_name) > L4_NVS_HANDLER_NS_MAX_LEN) {
        return L4_NVS_HANDLER_ERR_INVALID_NAMESPACE;
    }
    if (s_backend.get_json == NULL) {
        ESP_LOGE(TAG, "backend get_json callback is NULL");
        return L4_NVS_HANDLER_ERR_NVS_ACCESS;
    }

    /* Run materializer before reading the namespace */
    run_materializer(namespace_name);

    l4_nvs_handler_partition_t partition = infer_partition(namespace_name);

    esp_err_t err = s_backend.get_json(partition, namespace_name,
                                        out_json_array, s_backend.ctx);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return L4_NVS_HANDLER_ERR_NAMESPACE_NOT_FOUND;
    }
    if (err == ESP_ERR_NO_MEM) {
        return L4_NVS_HANDLER_ERR_MEMORY;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "get_json failed (0x%x) ns='%s'", err, namespace_name);
        return L4_NVS_HANDLER_ERR_NVS_ACCESS;
    }

    return L4_NVS_HANDLER_OK;
}

const char *l4_nvs_handler_get_error_response(l4_nvs_handler_error_t err)
{
    switch (err) {
    case L4_NVS_HANDLER_OK:                    return "ok";
    case L4_NVS_HANDLER_ERR_INVALID_PARTITION: return "invalid partition";
    case L4_NVS_HANDLER_ERR_MISSING_NAMESPACE: return "missing namespace";
    case L4_NVS_HANDLER_ERR_INVALID_NAMESPACE: return "invalid namespace";
    case L4_NVS_HANDLER_ERR_INVALID_KEY:       return "invalid key";
    case L4_NVS_HANDLER_ERR_INVALID_TYPE:      return "invalid type";
    case L4_NVS_HANDLER_ERR_INVALID_VALUE:     return "invalid value";
    case L4_NVS_HANDLER_ERR_MISSING_FIELD:     return "missing required field";
    case L4_NVS_HANDLER_ERR_INVALID_STRUCTURE: return "invalid structure";
    case L4_NVS_HANDLER_ERR_NAMESPACE_NOT_FOUND: return "namespace not found";
    case L4_NVS_HANDLER_ERR_NVS_FULL:          return "nvs full";
    case L4_NVS_HANDLER_ERR_NVS_ACCESS:        return "nvs access error";
    case L4_NVS_HANDLER_ERR_MEMORY:            return "memory error";
    case L4_NVS_HANDLER_ERR_UNKNOWN:
    default:                                    return "unknown error";
    }
}

esp_err_t l4_nvs_handler_format_error_json(
    l4_nvs_handler_error_t err,
    const char *detail,
    char *buf,
    size_t buf_size)
{
    if (buf == NULL || buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int http_status = l4_nvs_handler_error_to_http_status(err);
    const char *error_str = l4_nvs_handler_get_error_response(err);
    int written;

    if (detail != NULL && detail[0] != '\0') {
        written = snprintf(buf, buf_size,
                           "{\"status\":%d,\"error\":\"%s\",\"detail\":\"%s\"}",
                           http_status, error_str, detail);
    } else {
        written = snprintf(buf, buf_size,
                           "{\"status\":%d,\"error\":\"%s\"}",
                           http_status, error_str);
    }

    if (written < 0 || (size_t)written >= buf_size) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

int l4_nvs_handler_error_to_http_status(l4_nvs_handler_error_t err)
{
    switch (err) {
    case L4_NVS_HANDLER_OK:
        return 200;

    case L4_NVS_HANDLER_ERR_INVALID_PARTITION:
    case L4_NVS_HANDLER_ERR_MISSING_NAMESPACE:
    case L4_NVS_HANDLER_ERR_INVALID_NAMESPACE:
    case L4_NVS_HANDLER_ERR_INVALID_KEY:
    case L4_NVS_HANDLER_ERR_INVALID_TYPE:
    case L4_NVS_HANDLER_ERR_INVALID_VALUE:
    case L4_NVS_HANDLER_ERR_MISSING_FIELD:
    case L4_NVS_HANDLER_ERR_INVALID_STRUCTURE:
        return 400;

    case L4_NVS_HANDLER_ERR_NAMESPACE_NOT_FOUND:
        return 404;

    case L4_NVS_HANDLER_ERR_NVS_FULL:
        return 507;

    case L4_NVS_HANDLER_ERR_NVS_ACCESS:
    case L4_NVS_HANDLER_ERR_MEMORY:
    case L4_NVS_HANDLER_ERR_UNKNOWN:
    default:
        return 500;
    }
}

int l4_nvs_handler_error_to_mqtt_status(l4_nvs_handler_error_t err)
{
    /* Mirror the HTTP mapping for consistency with the /res topic style */
    return l4_nvs_handler_error_to_http_status(err);
}
