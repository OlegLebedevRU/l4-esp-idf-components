/**
 * @file l4_nvs_handler.h
 * @brief Generic NVS record handler for ESP-IDF 5.x.
 *
 * Validates and applies JSON NVS records of the form {p?, ns, k, t, v},
 * infers the NVS partition from the namespace prefix when the partition field
 * is absent, and exposes HTTP / MQTT status-code helpers and an error-JSON
 * formatter compatible with the existing /res MQTT response style.
 *
 * Transport details (HTTP server, MQTT client, task / message parsing) are
 * intentionally outside the scope of this component.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Error codes
 * ========================================================================= */

/**
 * @brief Handler-level error codes.
 *
 * Values < L4_NVS_HANDLER_ERR_NVS_FULL are validation errors that map to
 * HTTP 4xx / MQTT 4xx.  Values >= L4_NVS_HANDLER_ERR_NVS_FULL are storage /
 * system errors that map to HTTP 5xx / MQTT 5xx.
 */
typedef enum {
    L4_NVS_HANDLER_OK                    = 0,  /**< Success                        */
    L4_NVS_HANDLER_ERR_INVALID_PARTITION = 1,  /**< Unknown or invalid partition    */
    L4_NVS_HANDLER_ERR_MISSING_NAMESPACE = 2,  /**< "ns" field absent               */
    L4_NVS_HANDLER_ERR_INVALID_NAMESPACE = 3,  /**< Namespace value invalid/too long */
    L4_NVS_HANDLER_ERR_INVALID_KEY       = 4,  /**< Key absent, empty or too long    */
    L4_NVS_HANDLER_ERR_INVALID_TYPE      = 5,  /**< Unsupported "t" value            */
    L4_NVS_HANDLER_ERR_INVALID_VALUE     = 6,  /**< Value cannot be parsed for type  */
    L4_NVS_HANDLER_ERR_MISSING_FIELD     = 7,  /**< Required field absent            */
    L4_NVS_HANDLER_ERR_INVALID_STRUCTURE = 8,  /**< JSON object malformed            */
    L4_NVS_HANDLER_ERR_NAMESPACE_NOT_FOUND = 9,/**< Namespace not in NVS storage     */
    L4_NVS_HANDLER_ERR_NVS_FULL         = 10,  /**< NVS partition is full            */
    L4_NVS_HANDLER_ERR_NVS_ACCESS       = 11,  /**< Low-level NVS operation failed   */
    L4_NVS_HANDLER_ERR_MEMORY           = 12,  /**< Memory allocation failure        */
    L4_NVS_HANDLER_ERR_UNKNOWN          = 13,  /**< Unclassified error               */
} l4_nvs_handler_error_t;

/* =========================================================================
 * Partition enum
 * ========================================================================= */

/**
 * @brief Logical NVS partition identifiers.
 *
 * Downstream backends map these to their concrete partition label strings.
 */
typedef enum {
    L4_NVS_HANDLER_PART_FACTORY = 0, /**< Factory / read-only defaults    */
    L4_NVS_HANDLER_PART_CFG,         /**< User configuration partition     */
    L4_NVS_HANDLER_PART_DB,          /**< Runtime database partition       */
    L4_NVS_HANDLER_PART_LOG,         /**< Logging / audit partition        */
} l4_nvs_handler_partition_t;

/* =========================================================================
 * Parsed record
 * ========================================================================= */

/** Maximum length of an NVS namespace or key name (NVS_KEY_NAME_MAX_SIZE - 1). */
#define L4_NVS_HANDLER_NS_MAX_LEN    15
#define L4_NVS_HANDLER_KEY_MAX_LEN   15

/** Supported type strings. */
#define L4_NVS_HANDLER_TYPE_STR  "str"
#define L4_NVS_HANDLER_TYPE_I8   "i8"
#define L4_NVS_HANDLER_TYPE_U8   "u8"
#define L4_NVS_HANDLER_TYPE_I16  "i16"
#define L4_NVS_HANDLER_TYPE_U16  "u16"
#define L4_NVS_HANDLER_TYPE_I32  "i32"
#define L4_NVS_HANDLER_TYPE_U32  "u32"

/**
 * @brief Parsed and validated NVS record.
 *
 * Filled by l4_nvs_handler_validate_and_parse_record().
 *
 * @note str_value is a non-owning pointer into the source cJSON tree and is
 *       valid only while that cJSON object is alive.  Numeric records set
 *       num_value instead.
 */
typedef struct {
    l4_nvs_handler_partition_t  partition;                          /**< Resolved partition           */
    char                        namespace_name[L4_NVS_HANDLER_NS_MAX_LEN + 1];  /**< NVS namespace  */
    char                        key[L4_NVS_HANDLER_KEY_MAX_LEN + 1];            /**< NVS key        */
    char                        type_str[8];                        /**< Original type string         */
    bool                        is_str;                             /**< true if type == "str"        */
    bool                        is_delete_sentinel;                 /**< true if str erase-on-empty   */
    const char                 *str_value;                          /**< String value (borrowed ptr)  */
    int32_t                     num_value;                          /**< Numeric value (signed)       */
} l4_nvs_handler_record_t;

/* =========================================================================
 * Backend callback interface
 * ========================================================================= */

/**
 * @brief Backend callbacks for low-level NVS operations.
 *
 * Downstream projects implement these to wire the handler to their concrete
 * NVS layer (e.g. project_nvs.c, application NVS driver, etc.) without introducing
 * app-specific includes into this component.
 *
 * All callbacks receive the opaque @p ctx pointer registered in the struct.
 * A callback may be NULL if that operation is not supported; the handler will
 * return L4_NVS_HANDLER_ERR_NVS_ACCESS when a NULL callback is invoked.
 */
typedef struct {
    /**
     * Translate a partition label string (e.g. "cfg", "db1") into the
     * corresponding l4_nvs_handler_partition_t enum value.
     * Return ESP_ERR_NOT_FOUND if the string is not recognised.
     */
    esp_err_t (*partition_from_str)(const char *partition_str,
                                    l4_nvs_handler_partition_t *out_partition,
                                    void *ctx);

    /** Write a string value to NVS. */
    esp_err_t (*set_str)(l4_nvs_handler_partition_t partition,
                         const char *namespace_name,
                         const char *key,
                         const char *value,
                         void *ctx);

    /**
     * Write a signed 32-bit integer value to NVS.
     * @p type_str carries the original type tag ("i8", "u8", …, "u32") so the
     * backend can apply range checks or choose the correct nvs_set_* variant.
     */
    esp_err_t (*set_number)(l4_nvs_handler_partition_t partition,
                            const char *namespace_name,
                            const char *type_str,
                            const char *key,
                            int32_t value,
                            void *ctx);

    /** Erase a single key from NVS. */
    esp_err_t (*erase_key)(l4_nvs_handler_partition_t partition,
                           const char *namespace_name,
                           const char *key,
                           void *ctx);

    /**
     * Return a heap-allocated JSON array string containing all key/value pairs
     * in the given namespace.  The caller takes ownership and must free() the
     * returned buffer.  Pass NULL for *out_json_array on error.
     */
    esp_err_t (*get_json)(l4_nvs_handler_partition_t partition,
                          const char *namespace_name,
                          char **out_json_array,
                          void *ctx);

    /** Opaque context pointer forwarded to every callback. */
    void *ctx;
} l4_nvs_handler_backend_t;

/* =========================================================================
 * Namespace materializer
 * ========================================================================= */

/**
 * @brief Callback type for namespace materializers.
 *
 * A materializer ensures that NVS defaults for a specific namespace exist
 * before a GET or SET operation accesses it.  Downstream projects register
 * app-specific materializers; none are hardcoded in this component.
 *
 * @param ctx  Opaque context pointer provided at registration time.
 * @return     ESP_OK on success; any other value is treated as a non-fatal
 *             warning (the operation still proceeds).
 */
typedef esp_err_t (*l4_nvs_handler_materialize_fn_t)(void *ctx);

/**
 * @brief Register a materializer for a given NVS namespace.
 *
 * Registrations are stored in a fixed-size table whose capacity is controlled
 * by CONFIG_L4_NVS_HANDLER_MAX_MATERIALIZERS (Kconfig).  Registering the same
 * namespace name twice overwrites the previous entry.
 *
 * @param namespace_name  NVS namespace this materializer covers.
 * @param fn              Materializer callback (must not be NULL).
 * @param ctx             Opaque context forwarded to the callback.
 * @return ESP_OK on success, ESP_ERR_NO_MEM if the table is full.
 */
esp_err_t l4_nvs_handler_register_namespace_materializer(
    const char *namespace_name,
    l4_nvs_handler_materialize_fn_t fn,
    void *ctx);

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * @brief Initialise the handler with a backend implementation.
 *
 * Must be called once before any other l4_nvs_handler_* function.
 * Calls every registered namespace materializer in registration order.
 *
 * @param backend  Pointer to a populated backend struct (must not be NULL).
 *                 The struct is copied by value; the caller may release it
 *                 after this call returns.
 * @return ESP_OK on success.
 */
esp_err_t l4_nvs_handler_init(const l4_nvs_handler_backend_t *backend);

/**
 * @brief Validate and parse a single JSON record object.
 *
 * The expected record shape is:
 * @code
 *   { "p": "cfg",        // optional partition label
 *     "ns": "cfg_wifi",  // namespace  (required, max 15 chars)
 *     "k":  "ssid",      // key        (required, max 15 chars)
 *     "t":  "str",       // type       (required: str|i8|u8|i16|u16|i32|u32)
 *     "v":  "MyNet"      // value      (required; numbers accepted as JSON
 *                        //             number or numeric string)
 *   }
 * @endcode
 *
 * Partition inference (when "p" is absent):
 *  - namespace starts with "cfg_"  →  L4_NVS_HANDLER_PART_CFG
 *  - otherwise                     →  L4_NVS_HANDLER_PART_DB
 *
 * Delete sentinel: type == "str" && value == "" sets is_delete_sentinel.
 *
 * @param[in]  record_json  cJSON object representing one record.
 * @param[out] out_record   Filled on success; str_value borrows from record_json.
 * @return L4_NVS_HANDLER_OK on success, otherwise a validation error code.
 */
l4_nvs_handler_error_t l4_nvs_handler_validate_and_parse_record(
    const cJSON *record_json,
    l4_nvs_handler_record_t *out_record);

/**
 * @brief Apply a JSON array of SET records.
 *
 * Iterates through each element of @p records_array, calls
 * l4_nvs_handler_validate_and_parse_record(), then dispatches to the
 * appropriate backend callback (set_str, set_number, or erase_key).
 * Stops on the first error and returns the corresponding error code.
 *
 * @param records_array  cJSON array of record objects.
 * @return L4_NVS_HANDLER_OK if all records were applied successfully.
 */
l4_nvs_handler_error_t l4_nvs_handler_set_records(const cJSON *records_array);

/**
 * @brief Retrieve all records for a namespace as a JSON array string.
 *
 * Infers the partition from the namespace prefix, calls the registered
 * namespace materializer (if any), then delegates to the backend's get_json
 * callback.
 *
 * @param[in]  namespace_name  NVS namespace to query.
 * @param[out] out_json_array  On success, points to a heap-allocated JSON
 *                             array string.  The caller must free() this buffer.
 *                             Set to NULL on failure.
 * @return L4_NVS_HANDLER_OK on success.
 */
l4_nvs_handler_error_t l4_nvs_handler_get_records(
    const char *namespace_name,
    char **out_json_array);

/**
 * @brief Return a short, human-readable description of an error code.
 *
 * The returned string is a static constant; do not free() it.
 *
 * @param err  Handler error code.
 * @return Pointer to a null-terminated string describing the error.
 */
const char *l4_nvs_handler_get_error_response(l4_nvs_handler_error_t err);

/**
 * @brief Format an error response JSON into a caller-provided buffer.
 *
 * Produces a JSON object compatible with the existing MQTT /res response
 * style:
 * @code
 *   {"status": 400, "error": "invalid type", "detail": "t=xyz"}
 * @endcode
 *
 * @param err       Handler error code.
 * @param detail    Optional extra context string (may be NULL).
 * @param buf       Output buffer.
 * @param buf_size  Size of output buffer in bytes.
 * @return ESP_OK if the JSON was written; ESP_ERR_NO_MEM if the buffer is too small.
 */
esp_err_t l4_nvs_handler_format_error_json(
    l4_nvs_handler_error_t err,
    const char *detail,
    char *buf,
    size_t buf_size);

/**
 * @brief Map a handler error code to an HTTP status code.
 *
 * Validation errors (invalid partition/namespace/key/type/value/structure,
 * missing fields) map to 400.  Namespace-not-found maps to 404.  Storage and
 * system errors map to 500 (NVS full maps to 507).  OK maps to 200.
 *
 * @param err  Handler error code.
 * @return HTTP status integer (e.g. 200, 400, 404, 500, 507).
 */
int l4_nvs_handler_error_to_http_status(l4_nvs_handler_error_t err);

/**
 * @brief Map a handler error code to an MQTT response status code.
 *
 * Uses the same numeric convention as the HTTP mapping for consistency with
 * the existing /res topic response style used by downstream projects.
 *
 * @param err  Handler error code.
 * @return MQTT status integer (same scale as HTTP status codes).
 */
int l4_nvs_handler_error_to_mqtt_status(l4_nvs_handler_error_t err);

#ifdef __cplusplus
}
#endif
