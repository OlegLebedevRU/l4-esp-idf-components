/**
 * @file l4_nvs_store.h
 * @brief Low-level reusable NVS store layer for ESP-IDF 5.x.
 *
 * Provides mutex-protected, partition-aware NVS read/write/erase operations
 * and a JSON namespace dump.  Extracted from downstream projects so that
 * multiple applications can share one implementation.
 *
 * ## Partition model
 *
 * Four logical partitions are supported, identified by
 * @c l4_nvs_handler_partition_t (shared with l4_nvs_handler):
 *
 *   | Enum                         | Default NVS label |
 *   |------------------------------|-------------------|
 *   | L4_NVS_HANDLER_PART_FACTORY  | fctry             |
 *   | L4_NVS_HANDLER_PART_CFG      | cfg               |
 *   | L4_NVS_HANDLER_PART_DB       | db1               |
 *   | L4_NVS_HANDLER_PART_LOG      | log               |
 *
 * Partition labels are configurable via Kconfig
 * (CONFIG_L4_NVS_STORE_PART_LABEL_*).
 *
 * ## Partition init and lazy initialisation
 *
 * Partitions are initialised on demand: the first call that opens a namespace
 * triggers @c nvs_flash_init_partition() (or the encrypted variant when
 * CONFIG_NVS_ENCRYPTION is enabled) automatically.
 *
 * Explicit init helpers are also provided for code that prefers to init
 * partitions explicitly at startup.
 *
 * ## Backend factory
 *
 * @c l4_nvs_handler_make_store_backend() returns a fully-populated
 * @c l4_nvs_handler_backend_t wired to this store layer.  Pass it to
 * @c l4_nvs_handler_init() to avoid boilerplate callback definitions:
 *
 * @code
 *   l4_nvs_handler_init_with_store_backend();
 * @endcode
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"
#include "l4_nvs_handler.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Partition label helpers
 * ========================================================================= */

/**
 * @brief Return the NVS flash partition label for a logical partition.
 *
 * Returns the compile-time label configured via Kconfig
 * (CONFIG_L4_NVS_STORE_PART_LABEL_*).
 *
 * @param partition  Logical partition enum value.
 * @return Pointer to a null-terminated partition label string, or NULL if
 *         @p partition is out of range.
 */
const char *l4_nvs_store_partition_label(l4_nvs_handler_partition_t partition);

/**
 * @brief Resolve a partition label string to a logical partition enum.
 *
 * Recognised aliases:
 *   - "factory", "fctry"  → L4_NVS_HANDLER_PART_FACTORY
 *   - "cfg"               → L4_NVS_HANDLER_PART_CFG
 *   - "db", "db1"         → L4_NVS_HANDLER_PART_DB
 *   - "log"               → L4_NVS_HANDLER_PART_LOG
 *
 * In addition, the Kconfig-derived label strings are also accepted.
 *
 * @param[in]  str            Partition label string.
 * @param[out] out_partition  Receives the resolved enum value on success.
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if unrecognised,
 *         ESP_ERR_INVALID_ARG if either pointer is NULL.
 */
esp_err_t l4_nvs_store_partition_from_str(const char *str,
                                           l4_nvs_handler_partition_t *out_partition);

/* =========================================================================
 * Partition init helpers
 * ========================================================================= */

/**
 * @brief Initialise a logical NVS partition explicitly.
 *
 * Under CONFIG_NVS_ENCRYPTION the function attempts secure init first,
 * using the key partition whose subtype is
 * ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS.  Falls back to plain init if no
 * suitable key partition is found.
 *
 * @param partition  Logical partition to initialise.
 * @return ESP_OK on success.
 */
esp_err_t l4_nvs_store_init_partition(l4_nvs_handler_partition_t partition);

/** @brief Init the factory partition explicitly. */
static inline esp_err_t l4_nvs_store_init_factory_partition(void)
{
    return l4_nvs_store_init_partition(L4_NVS_HANDLER_PART_FACTORY);
}

/** @brief Init the cfg partition explicitly. */
static inline esp_err_t l4_nvs_store_init_cfg_partition(void)
{
    return l4_nvs_store_init_partition(L4_NVS_HANDLER_PART_CFG);
}

/** @brief Init the db partition explicitly. */
static inline esp_err_t l4_nvs_store_init_db_partition(void)
{
    return l4_nvs_store_init_partition(L4_NVS_HANDLER_PART_DB);
}

/** @brief Init the log partition explicitly. */
static inline esp_err_t l4_nvs_store_init_log_partition(void)
{
    return l4_nvs_store_init_partition(L4_NVS_HANDLER_PART_LOG);
}

/* =========================================================================
 * Typed reads
 * ========================================================================= */

/**
 * @brief Read a uint8_t value from NVS.
 *
 * The operation is mutex-protected.  If the target partition is not yet
 * initialised (ESP_ERR_NVS_NOT_INITIALIZED) it is initialised lazily before
 * retrying.
 *
 * @param partition  Logical partition.
 * @param ns         NVS namespace name.
 * @param key        NVS key name.
 * @param[out] out   Receives the value on success.
 * @return ESP_OK on success, otherwise an NVS or system error code.
 */
esp_err_t l4_nvs_store_read_u8(l4_nvs_handler_partition_t partition,
                                const char *ns,
                                const char *key,
                                uint8_t *out);

/**
 * @brief Read a int32_t value from NVS.
 * @see l4_nvs_store_read_u8 for parameter and error-code documentation.
 */
esp_err_t l4_nvs_store_read_i32(l4_nvs_handler_partition_t partition,
                                 const char *ns,
                                 const char *key,
                                 int32_t *out);

/**
 * @brief Read a uint32_t value from NVS.
 * @see l4_nvs_store_read_u8 for parameter and error-code documentation.
 */
esp_err_t l4_nvs_store_read_u32(l4_nvs_handler_partition_t partition,
                                 const char *ns,
                                 const char *key,
                                 uint32_t *out);

/**
 * @brief Read a string value from NVS into a caller-supplied buffer.
 *
 * If the stored string (including the null terminator) is longer than
 * @p buf_len, ESP_ERR_NVS_INVALID_LENGTH is returned and @p buf is
 * unmodified.
 *
 * @param partition  Logical partition.
 * @param ns         NVS namespace name.
 * @param key        NVS key name.
 * @param[out] buf   Destination buffer.
 * @param buf_len    Size of @p buf in bytes.
 * @return ESP_OK on success, otherwise an NVS or system error code.
 */
esp_err_t l4_nvs_store_read_str(l4_nvs_handler_partition_t partition,
                                 const char *ns,
                                 const char *key,
                                 char *buf,
                                 size_t buf_len);

/* =========================================================================
 * Typed writes
 * ========================================================================= */

/**
 * @brief Write a string value to NVS and commit.
 *
 * @param partition  Logical partition.
 * @param ns         NVS namespace name.
 * @param key        NVS key name.
 * @param value      Null-terminated string to write.
 * @return ESP_OK on success, otherwise an NVS or system error code.
 */
esp_err_t l4_nvs_store_set_str(l4_nvs_handler_partition_t partition,
                                const char *ns,
                                const char *key,
                                const char *value);

/**
 * @brief Write a numeric value to NVS and commit.
 *
 * The type_str selects the NVS storage variant:
 *   "i8", "u8", "i16", "u16", "i32", "u32"
 *
 * The @p value is cast to the appropriate width before writing.
 *
 * @param partition  Logical partition.
 * @param ns         NVS namespace name.
 * @param type_str   Type tag string (see supported values above).
 * @param key        NVS key name.
 * @param value      Signed 32-bit value; cast to target type internally.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if type_str is unknown,
 *         otherwise an NVS or system error code.
 */
esp_err_t l4_nvs_store_set_number(l4_nvs_handler_partition_t partition,
                                   const char *ns,
                                   const char *type_str,
                                   const char *key,
                                   int32_t value);

/* =========================================================================
 * DB partition convenience helpers
 * ========================================================================= */

/**
 * @brief Read a string from the DB partition.
 *
 * Equivalent to l4_nvs_store_read_str(L4_NVS_HANDLER_PART_DB, ...).
 */
esp_err_t l4_nvs_store_get_str_from_db(const char *ns,
                                        const char *key,
                                        char *buf,
                                        size_t buf_len);

/**
 * @brief Read a uint32_t from the DB partition.
 *
 * Equivalent to l4_nvs_store_read_u32(L4_NVS_HANDLER_PART_DB, ...).
 */
esp_err_t l4_nvs_store_get_u32_from_db(const char *ns,
                                        const char *key,
                                        uint32_t *out);

/* =========================================================================
 * JSON namespace dump
 * ========================================================================= */

/**
 * @brief Dump all key/value pairs in a namespace as a JSON array string.
 *
 * The returned string has the form:
 * @code
 *   [{"k":"key1","t":"str","v":"value"},{"k":"key2","t":"u32","v":42}, ...]
 * @endcode
 *
 * At most CONFIG_L4_NVS_STORE_MAX_ENTRIES entries are included.  Unsupported
 * NVS types (blob, u64, i64) are silently skipped.
 *
 * The caller is responsible for free()-ing the returned buffer.
 *
 * @param partition       Logical partition.
 * @param ns              NVS namespace name.
 * @param[out] out_json   On success, points to a heap-allocated JSON string.
 *                        Set to NULL on failure.
 * @return ESP_OK on success, ESP_ERR_NVS_NOT_FOUND if the namespace does not
 *         exist, ESP_ERR_NO_MEM on allocation failure, otherwise an NVS error.
 */
esp_err_t l4_nvs_store_get_json(l4_nvs_handler_partition_t partition,
                                 const char *ns,
                                 char **out_json);

/* =========================================================================
 * Key erase
 * ========================================================================= */

/**
 * @brief Erase a single key from NVS and commit.
 *
 * @param partition  Logical partition.
 * @param ns         NVS namespace name.
 * @param key        NVS key to erase.
 * @return ESP_OK on success, otherwise an NVS or system error code.
 */
esp_err_t l4_nvs_store_erase_key(l4_nvs_handler_partition_t partition,
                                  const char *ns,
                                  const char *key);

/* =========================================================================
 * Backend factory for l4_nvs_handler
 * ========================================================================= */

/**
 * @brief Build a fully-populated l4_nvs_handler_backend_t backed by this
 *        store layer.
 *
 * All five backend callbacks are wired to the corresponding
 * l4_nvs_store_* functions.  The returned struct can be passed directly to
 * l4_nvs_handler_init():
 *
 * @code
 *   l4_nvs_handler_backend_t backend = l4_nvs_handler_make_store_backend();
 *   l4_nvs_handler_init(&backend);
 * @endcode
 *
 * @return Populated l4_nvs_handler_backend_t (ctx field is NULL).
 */
l4_nvs_handler_backend_t l4_nvs_handler_make_store_backend(void);

/**
 * @brief Convenience: build the store backend and call l4_nvs_handler_init().
 *
 * Equivalent to:
 * @code
 *   l4_nvs_handler_backend_t b = l4_nvs_handler_make_store_backend();
 *   return l4_nvs_handler_init(&b);
 * @endcode
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG from l4_nvs_handler_init.
 */
esp_err_t l4_nvs_handler_init_with_store_backend(void);

#ifdef __cplusplus
}
#endif
