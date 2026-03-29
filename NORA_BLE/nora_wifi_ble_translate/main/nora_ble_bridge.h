#pragma once

/**
 * Send a BLE notification to the connected central (iPhone/PC).
 * No-op if no device is connected.
 * @param data  NULL-terminated UTF-8 string to send
 */
void ble_notify_send(const char *data);
