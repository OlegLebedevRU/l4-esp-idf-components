# l4_nvs_handler

Generic NVS record handler component for ESP-IDF 5.x.

## Purpose

`l4_nvs_handler` captures the common NVS record handler layer that evolved in
parallel across multiple downstream projects (`l4-hmi`, `siplite`).  It
provides a single, transport-agnostic implementation of:

- JSON record validation and parsing (`{p?, ns, k, t, v}` shape)
- Partition inference from namespace prefix
- GET and SET orchestration via a pluggable backend interface
- Delete-sentinel compatibility for backwards-compatible key erasure
- HTTP and MQTT status-code helpers
- MQTT `/res`-compatible error JSON formatting

The component now also includes a **reusable low-level NVS store layer**
(`l4_nvs_store.h` / `src/l4_nvs_store.c`) that implements the concrete
partition init, mutex-protected read/write/erase, and JSON dump operations
that were previously duplicated across downstream applications.

Transport details (HTTP server, MQTT client, message routing) are
**intentionally out of scope** and remain in the downstream project.

---

## Record format

Each NVS record is a JSON object with the following fields:

| Field | Required | Description |
|-------|----------|-------------|
| `p`   | No       | Partition label (e.g. `"cfg"`, `"db1"`). Inferred when absent. |
| `ns`  | Yes      | NVS namespace name (max 15 characters). |
| `k`   | Yes      | NVS key name (max 15 characters). |
| `t`   | Yes      | Value type: `str`, `i8`, `u8`, `i16`, `u16`, `i32`, `u32`. |
| `v`   | Yes      | Value. Numbers accepted as JSON number or numeric string. |

Example SET array:

```json
[
  { "ns": "cfg_wifi", "k": "ssid",     "t": "str", "v": "MyNetwork" },
  { "ns": "cfg_wifi", "k": "channel",  "t": "i8",  "v": 6 },
  { "p": "db1",       "ns": "runtime", "k": "uptime", "t": "u32", "v": "0" }
]
```

---

## Partition inference policy

When the `p` field is **absent**, the component infers the partition from the
namespace name:

| Namespace prefix | Inferred partition |
|------------------|--------------------|
| `cfg_`           | `L4_NVS_HANDLER_PART_CFG` |
| *(anything else)*| `L4_NVS_HANDLER_PART_DB`  |

When `p` is **present**, the backend's `partition_from_str` callback translates
the label string to the `l4_nvs_handler_partition_t` enum value.

---

## Delete sentinel compatibility

For backwards compatibility with existing MQTT and HTTP clients, an empty
string value with type `str` is treated as a **delete sentinel** rather than
writing an empty string to NVS:

```json
{ "ns": "cfg_wifi", "k": "ssid", "t": "str", "v": "" }
```

This record erases the key `ssid` from namespace `cfg_wifi` instead of
writing an empty value.

---

## Backend callback adapter pattern

The component does **not** depend on any project-specific NVS driver.  Instead,
downstream projects register a `l4_nvs_handler_backend_t` struct with
callbacks that wire the handler to their low-level NVS layer:

```c
// 1. Implement the backend callbacks for your project
static esp_err_t my_partition_from_str(const char *str,
                                        l4_nvs_handler_partition_t *out,
                                        void *ctx)
{
    if (strcmp(str, "cfg") == 0)  { *out = L4_NVS_HANDLER_PART_CFG; return ESP_OK; }
    if (strcmp(str, "db1") == 0)  { *out = L4_NVS_HANDLER_PART_DB;  return ESP_OK; }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t my_set_str(l4_nvs_handler_partition_t part,
                             const char *ns, const char *key,
                             const char *val, void *ctx)
{
    // call your nvs_set_str wrapper here
    return my_nvs_set_str(part, ns, key, val);
}

// ... implement set_number, erase_key, get_json similarly ...

// 2. Register and initialise
static const l4_nvs_handler_backend_t s_nvs_backend = {
    .partition_from_str = my_partition_from_str,
    .set_str            = my_set_str,
    .set_number         = my_set_number,
    .erase_key          = my_erase_key,
    .get_json           = my_get_json,
    .ctx                = NULL,
};

void app_main(void)
{
    l4_nvs_handler_init(&s_nvs_backend);
}
```

---

## Materializer registration pattern

A **namespace materializer** is a callback that populates NVS defaults for a
specific namespace before the first GET or SET operation.  This replaces the
app-specific namespace knowledge that was previously hardcoded in
previous implementations.

```c
static esp_err_t my_cfg_wifi_materialize(void *ctx)
{
    // Write default values for cfg_wifi if they do not already exist
    return my_nvs_write_defaults_cfg_wifi();
}

// Register before calling l4_nvs_handler_init()
l4_nvs_handler_register_namespace_materializer(
    "cfg_wifi",
    my_cfg_wifi_materialize,
    NULL);

l4_nvs_handler_init(&s_nvs_backend);
```

The component stores materializers in a fixed-size table.  The capacity is
controlled by `CONFIG_L4_NVS_HANDLER_MAX_MATERIALIZERS` (Kconfig, default 8).

---

## Using the component from another ESP-IDF project

### Option A — Git dependency in `idf_component.yml`

In your project's `main/idf_component.yml` (or the consuming component's
`idf_component.yml`):

```yaml
dependencies:
  l4_nvs_handler:
    git: https://github.com/OlegLebedevRU/l4-esp-idf-components.git
    path: components/l4_nvs_handler
```

Then run:

```sh
idf.py reconfigure
idf.py build
```

### Option B — Local copy in `components/`

Copy `components/l4_nvs_handler/` into your project's `components/` directory
and add it to your component's `CMakeLists.txt` `REQUIRES` list:

```cmake
idf_component_register(
    SRCS "my_app.c"
    INCLUDE_DIRS "."
    REQUIRES l4_nvs_handler
)
```

---

## NVS store layer (`l4_nvs_store`)

### Overview

`l4_nvs_store.h` / `src/l4_nvs_store.c` is the reusable low-level NVS store
layer extracted from downstream applications.  It handles:

- **Partition init** — plain or encrypted (CONFIG_NVS_ENCRYPTION), with
  automatic erase-and-reinit on `ESP_ERR_NVS_NO_FREE_PAGES` /
  `ESP_ERR_NVS_NEW_VERSION_FOUND`.
- **Lazy init** — the first call that opens a namespace triggers partition
  init automatically on `ESP_ERR_NVS_NOT_INITIALIZED`.
- **Mutex protection** — a single FreeRTOS mutex serialises all NVS
  open/read/write/enumerate operations.
- **Typed reads**: `read_u8`, `read_i32`, `read_u32`, `read_str`.
- **Typed writes**: `set_str`, `set_number` (dispatches to the correct
  `nvs_set_*` variant for `i8`/`u8`/`i16`/`u16`/`i32`/`u32`).
- **DB convenience helpers**: `get_str_from_db`, `get_u32_from_db`.
- **JSON namespace dump** (`get_json`) — iterates all supported-type entries
  in a namespace and returns a heap-allocated `[{"k":…,"t":…,"v":…}]` string.
  Protected by `CONFIG_L4_NVS_STORE_MAX_ENTRIES` (default 512).
- **Key erase**: `erase_key`.

### Partition model

Four logical partitions are identified by `l4_nvs_handler_partition_t`
(shared with `l4_nvs_handler`):

| Enum                        | Default NVS label | Config key                           |
|-----------------------------|-------------------|--------------------------------------|
| `L4_NVS_HANDLER_PART_FACTORY` | `fctry`         | `CONFIG_L4_NVS_STORE_PART_LABEL_FACTORY` |
| `L4_NVS_HANDLER_PART_CFG`     | `cfg`           | `CONFIG_L4_NVS_STORE_PART_LABEL_CFG`    |
| `L4_NVS_HANDLER_PART_DB`      | `db1`           | `CONFIG_L4_NVS_STORE_PART_LABEL_DB`     |
| `L4_NVS_HANDLER_PART_LOG`     | `log`           | `CONFIG_L4_NVS_STORE_PART_LABEL_LOG`    |

Recognised `partition_from_str` aliases:

- `"factory"`, `"fctry"` → `L4_NVS_HANDLER_PART_FACTORY`
- `"cfg"` → `L4_NVS_HANDLER_PART_CFG`
- `"db"`, `"db1"` → `L4_NVS_HANDLER_PART_DB`
- `"log"` → `L4_NVS_HANDLER_PART_LOG`

### Quick start — using the built-in store backend

```c
#include "l4_nvs_store.h"

void app_main(void)
{
    // Register materializers first (if any), then:
    l4_nvs_handler_init_with_store_backend();

    // Now l4_nvs_handler_set_records / l4_nvs_handler_get_records work.
    // You can also call the store API directly:
    uint32_t val = 0;
    l4_nvs_store_read_u32(L4_NVS_HANDLER_PART_DB, "runtime", "counter", &val);
}
```

### Migration path from a custom backend

If your project already defines a custom `l4_nvs_handler_backend_t`, you can
replace the boilerplate callbacks with the store layer at your own pace:

```c
// Before: full custom backend
static const l4_nvs_handler_backend_t s_backend = {
    .partition_from_str = my_partition_from_str,  // 20+ lines
    .set_str            = my_set_str,             // 10+ lines each
    .set_number         = my_set_number,
    .erase_key          = my_erase_key,
    .get_json           = my_get_json,
};

// After: one call
l4_nvs_handler_init_with_store_backend();
```

Downstream app-specific materializers remain in the application:

```c
l4_nvs_handler_register_namespace_materializer(
    "cfg_wifi", my_cfg_wifi_materialize, NULL);
l4_nvs_handler_init_with_store_backend();
```

---

## What is intentionally out of scope

- HTTP server or HTTP request handlers
- MQTT client or topic subscription/publish
- Task or message-queue parsers
- Any app-specific namespace names, defaults, or schemas
- Any project-specific headers (e.g. application NVS drivers, config repo headers, or third-party client headers)
- `catalog_store.c` / raw db1 flash catalog persistence

---

## CMake dependency

The component depends on ESP-IDF `json` (cJSON), `nvs_flash`, and `freertos`.
This is declared in `CMakeLists.txt` and requires no manual configuration.

---

## Kconfig options

### L4 NVS Handler

| Option | Default | Description |
|--------|---------|-------------|
| `CONFIG_L4_NVS_HANDLER_MAX_MATERIALIZERS` | 8 | Size of the materializer registration table |

### L4 NVS Store

| Option | Default | Description |
|--------|---------|-------------|
| `CONFIG_L4_NVS_STORE_PART_LABEL_FACTORY` | `fctry` | Flash partition label for factory partition |
| `CONFIG_L4_NVS_STORE_PART_LABEL_CFG`     | `cfg`   | Flash partition label for config partition |
| `CONFIG_L4_NVS_STORE_PART_LABEL_DB`      | `db1`   | Flash partition label for database partition |
| `CONFIG_L4_NVS_STORE_PART_LABEL_LOG`     | `log`   | Flash partition label for log partition |
| `CONFIG_L4_NVS_STORE_MAX_ENTRIES`         | 512     | Max NVS entries processed per JSON dump |
