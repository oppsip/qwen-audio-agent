#ifndef BLE_CONFIG_SERVER_H
#define BLE_CONFIG_SERVER_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the BLE configuration GATT server.
 * Must be called after nvs_flash_init() and before ble_config_server_start().
 * Initializes BT controller (BLE-only mode) and Bluedroid stack.
 */
esp_err_t ble_config_server_init(const char *device_name);

/**
 * Start BLE advertising so the phone can discover and connect.
 * The server accepts JSON commands over GATT Write characteristic
 * and sends responses via GATT Notify characteristic.
 */
esp_err_t ble_config_server_start(void);

/**
 * Stop advertising and disconnect any active BLE connection.
 * Safe to call even if not started.
 */
void ble_config_server_stop(void);

/**
 * Returns true if a phone is currently connected via BLE.
 */
bool ble_config_server_is_connected(void);

/**
 * Block until the phone sends a "reboot" command or the timeout elapses.
 * Returns true if the phone requested a reboot, false on timeout.
 *
 * @param timeout_ms  Maximum wait time in milliseconds (0 = wait forever)
 */
bool ble_config_server_wait_for_reboot(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* BLE_CONFIG_SERVER_H */
