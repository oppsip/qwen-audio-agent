#ifndef WEB_CONFIG_H
#define WEB_CONFIG_H

#include "esp_err.h"
#include "cJSON.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WEB_CONFIG_RUNTIME_TIMEOUT_MS  (10 * 60 * 1000)

typedef enum {
    WEB_CONFIG_MODE_SETUP = 0,   /* first config: wait until applied */
    WEB_CONFIG_MODE_RUNTIME,     /* long-press while running: 10 min */
} web_config_mode_t;

typedef enum {
    WEB_CONFIG_APPLIED = 0,      /* new values saved; caller should reboot */
    WEB_CONFIG_TIMEOUT,          /* no submit within the window */
    WEB_CONFIG_UNAVAILABLE,      /* platform URL empty or HTTP failed */
    WEB_CONFIG_BUSY,             /* another session already running */
} web_config_result_t;

bool web_config_is_enabled(void);

/**
 * Register with the platform using the factory MAC device ID.
 * Idempotent: if the unit is already known, the existing device password
 * is returned and stored locally.
 */
esp_err_t web_config_register(void);

/**
 * POST current schema+values, poll until the user submits (or timeout).
 * On WEB_CONFIG_APPLIED the new parameters are already in NVS.
 *
 * @param timeout_ms  0 = wait indefinitely (setup); runtime uses 10 minutes
 */
web_config_result_t web_config_run_session(web_config_mode_t mode,
                                           uint32_t timeout_ms);

/**
 * Schema API shared with the BLE config channel (main/ble_config_server.c).
 *
 * The BLE transport serves exactly the same { ui, values, contacts } document
 * the platform session carries, and applies submitted values through the same
 * web_config_apply_values() path, so the two channels can never drift apart.
 * A future web field is therefore visible to the mini program with no app
 * release.
 *
 * web_config_json_init() installs the PSRAM-preferred cJSON allocator; the
 * BLE side must call it once before it parses or prints JSON. The build
 * functions also call it, so they are safe on their own.
 *
 * The cJSON objects returned below are owned by the caller: release them with
 * cJSON_Delete(). They may be NULL on out-of-memory.
 */
void web_config_json_init(void);
cJSON *web_config_build_ui(void);
cJSON *web_config_build_values(void);
cJSON *web_config_build_contacts(void);

/**
 * Apply a { key: value } map. Keys are the same as build_values(); missing
 * keys keep the stored value, and an unchanged secret arrives as "" and also
 * keeps the stored value. "frs":1 resets to factory defaults.
 */
esp_err_t web_config_apply_values(const cJSON *values);

#ifdef __cplusplus
}
#endif

#endif /* WEB_CONFIG_H */
