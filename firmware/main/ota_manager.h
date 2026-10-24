#pragma once
#include "esp_err.h"

/* Check server for a newer firmware version and apply it if available.
 * Blocks until check completes (or times out).  If an update is applied,
 * the device reboots and this function never returns.
 * Returns ESP_OK if up-to-date or if OTA_MANIFEST_URL is empty. */
esp_err_t ota_manager_check_and_update(void);

/* Must be called early in app_main to confirm the running firmware is healthy.
 * If the device booted into a PENDING_VERIFY OTA image this cancels rollback;
 * if it booted normally this is a no-op. */
void ota_manager_mark_valid(void);
