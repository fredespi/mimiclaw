#pragma once

#include "esp_err.h"

/**
 * Perform OTA firmware update from a URL.
 * Downloads the firmware binary and applies it. Reboots on success.
 * If callback_url is provided, it is saved to SPIFFS so that after reboot
 * the device can POST back to signal completion.
 *
 * @param url  HTTP/HTTPS URL to the firmware .bin file
 * @return ESP_OK on success (device will reboot), error code otherwise
 */
esp_err_t ota_update_from_url(const char *url);

/**
 * Check for an OTA completion marker after reboot.
 * If found, POST the result back to the deploy server and delete the marker.
 * Call this after WiFi is connected.
 */
void ota_check_post_update(void);
