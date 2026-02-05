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
// Device Configuration Structure
// ============================================================================
struct DeviceConfig {
    const char* serial;         // MAC address without colons (e.g., "50547B815AFB")
    const char* name;           // Friendly name for logs
    BatteryType type;           // Battery type (LEAD_ACID or AGM for automatic mode)
    bool enabled;               // Enable monitoring for this device
};

#endif // TYPES_H
