/*
 * nvs.h - host-test stub, backed by the fake in-RAM flash in host_env.c.
 *
 * This is the load-bearing stub: the whole point of linking signal_store.c
 * and node_graph.c into the host binary is to drive their save/rollback paths
 * against an NVS that can be told to run out of space or to fail its next
 * write — states that are miserable to reproduce on a real box and are
 * exactly where the data-destroying bugs live. The API surface and the error
 * VALUES mirror the real nvs.h (see esp_err.h stub for why values must not
 * drift); the semantics implemented are the ones the firmware relies on:
 * per-namespace key/blob storage, "new copy must fit before the old one is
 * freed" on overwrite (the property that makes a full partition refuse even
 * shrinking rewrites), and entry-based accounting for nvs_get_stats().
 */
#ifndef DB_HOSTTEST_NVS_H
#define DB_HOSTTEST_NVS_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define ESP_ERR_NVS_BASE              0x1100
#define ESP_ERR_NVS_NOT_INITIALIZED   (ESP_ERR_NVS_BASE + 0x01)
#define ESP_ERR_NVS_NOT_FOUND         (ESP_ERR_NVS_BASE + 0x02)
#define ESP_ERR_NVS_TYPE_MISMATCH     (ESP_ERR_NVS_BASE + 0x03)
#define ESP_ERR_NVS_READ_ONLY         (ESP_ERR_NVS_BASE + 0x04)
#define ESP_ERR_NVS_NOT_ENOUGH_SPACE  (ESP_ERR_NVS_BASE + 0x05)
#define ESP_ERR_NVS_INVALID_HANDLE    (ESP_ERR_NVS_BASE + 0x07)
#define ESP_ERR_NVS_INVALID_LENGTH    (ESP_ERR_NVS_BASE + 0x0c)
#define ESP_ERR_NVS_NO_FREE_PAGES     (ESP_ERR_NVS_BASE + 0x0d)
#define ESP_ERR_NVS_NEW_VERSION_FOUND (ESP_ERR_NVS_BASE + 0x10)

typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY = 0, NVS_READWRITE = 1 } nvs_open_mode_t;
typedef enum {
    NVS_TYPE_BLOB = 0x42,
    NVS_TYPE_ANY  = 0xff,
} nvs_type_t;

esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t mode,
                   nvs_handle_t *out_handle);
void      nvs_close(nvs_handle_t handle);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key,
                       const void *value, size_t length);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key,
                       void *out_value, size_t *length);
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key);
esp_err_t nvs_commit(nvs_handle_t handle);
esp_err_t nvs_find_key(nvs_handle_t handle, const char *key,
                       nvs_type_t *out_type);

/* ---- stats ---- */

typedef struct {
    size_t used_entries;
    size_t free_entries;
    size_t available_entries;
    size_t total_entries;
    size_t namespace_count;
} nvs_stats_t;

esp_err_t nvs_get_stats(const char *part_name, nvs_stats_t *nvs_stats);

/* ---- iteration ---- */

#define NVS_PART_NAME_MAX_SIZE 16
#define NVS_KEY_NAME_MAX_SIZE  16
#define NVS_NS_NAME_MAX_SIZE   16

typedef struct {
    char       namespace_name[NVS_NS_NAME_MAX_SIZE];
    char       key[NVS_KEY_NAME_MAX_SIZE];
    nvs_type_t type;
} nvs_entry_info_t;

typedef struct nvs_opaque_iterator_t *nvs_iterator_t;

esp_err_t nvs_entry_find(const char *part_name, const char *namespace_name,
                         nvs_type_t type, nvs_iterator_t *output_iterator);
esp_err_t nvs_entry_next(nvs_iterator_t *iterator);
esp_err_t nvs_entry_info(nvs_iterator_t iterator, nvs_entry_info_t *out_info);
void      nvs_release_iterator(nvs_iterator_t iterator);

#endif
