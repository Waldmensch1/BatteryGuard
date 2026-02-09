/**
 * Battery Guard Multi-Device Monitor - Type Definitions
 */

#ifndef TYPES_H
#define TYPES_H

#include <Arduino.h>

// ============================================================================
// Battery Type Definitions
// ============================================================================
enum BatteryType : uint8_t {
    LEAD_ACID = 0x01,           // Standard Lead Acid (6 writes)
    AGM = 0x02,                 // AGM Battery (6 writes)
    OTHER_INTELLIGENT = 0x03,   // Other + Intelligent (7 writes)
    OTHER_MANUAL = 0x04,        // Other + Manual mode (7 writes)
    LITHIUM = 0x05,             // Lithium Standard (6 writes)
    LITHIUM_INTELLIGENT = 0x06, // Lithium + Intelligent (7 writes)
    LITHIUM_MANUAL = 0x07       // Lithium + Manual (7 writes)
};

// ============================================================================
// Battery Status Definitions
// ============================================================================
// Based on empirical testing with actual app and hardware:
// - 13.1V: App shows "Batterie in Ordnung" → Byte[5]=0x01 (Motor off)
// - 13.5V: App shows "Ladevorgang" → Byte[5]=0x02 (Charging, voltage >13.3V)
// - Threshold: ~13.3V between off/on states
enum BatteryStatus : uint8_t {
    STATUS_UNKNOWN = 0x00,   // Unknown/Error state (never observed)
    STATUS_NORMAL = 0x01,    // Motor off, no charging (App: "Batterie in Ordnung")
    STATUS_CHARGING = 0x02   // Charging detected (App: "Ladevorgang")
};

// Status text conversion - centralized for consistency
// Shows charging state in clear format
inline const char* getBatteryStatusText(uint8_t status) {
    switch (status) {
        case STATUS_NORMAL:   return "Charge: off";
        case STATUS_CHARGING: return "Charge: on";
        case STATUS_UNKNOWN:  return "Charge: 0x00";
        default: {
            static char buf[16];
            sprintf(buf, "Charge: 0x%02X", status);
            return buf;
        }
    }
}

// MQTT version without "Charge:" prefix
inline const char* getBatteryStatusMqtt(uint8_t status) {
    switch (status) {
        case STATUS_NORMAL:   return "off";
        case STATUS_CHARGING: return "on";
        case STATUS_UNKNOWN:  return "0x00";
        default: {
            static char buf[16];
            sprintf(buf, "0x%02X", status);
            return buf;
        }
    }
}

// ============================================================================
// Device Configuration Structure
// ============================================================================
struct DeviceConfig {
    const char* serial;         // MAC address without colons (e.g., "50547B815AFB")
    const char* name;           // Friendly name for logs
    const char* mqttName;       // MQTT topic name (e.g., "main_battery")
    BatteryType type;           // Battery type (LEAD_ACID or AGM for automatic mode)
    bool enabled;               // Enable monitoring for this device
    const uint8_t* key;         // Pointer to AES key for this device
};

// ============================================================================
// Display Data Structure (Thread-Safe)
// ============================================================================
// Shared between BLE thread (Core 1) and Display thread (Core 0)
// Using volatile for cross-core visibility
#define MAX_MONITORS 4

struct DeviceDisplayData {
    volatile bool active;           // Device configured in config.h
    volatile bool connected;        // Currently connected via BLE
    char name[32];                  // Device name (copied once at init)
    char address[18];               // MAC address string (copied once at init)
    
    // Battery data - updated by BLE callbacks
    volatile float voltage;
    volatile int soc;
    volatile int temperature;
    volatile uint8_t status;
    volatile uint8_t rapidVoltageRise;
    volatile uint8_t rapidVoltageDrop;
    volatile unsigned long lastUpdate;  // millis() timestamp
};

#endif // TYPES_H
