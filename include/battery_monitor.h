/**
 * Battery Guard Multi-Device Monitor - Battery Monitor Class
 */

#ifndef BATTERY_MONITOR_H
#define BATTERY_MONITOR_H

#include <Arduino.h>
#include <NimBLEDevice.h>
#include "types.h"
#include "config.h"

// ============================================================================
// Device State Definitions
// ============================================================================
enum DeviceState {
    STATE_DISCONNECTED,     // Not connected
    STATE_SCANNING,         // Searching for device
    STATE_CONNECTING,       // Establishing connection
    STATE_HANDSHAKE,        // Sending handshake
    STATE_MONITORING,       // Receiving data
    STATE_COOLDOWN          // Waiting after failed retries
};

inline const char* stateToString(DeviceState state) {
    switch(state) {
        case STATE_DISCONNECTED: return "DISCONNECTED";
        case STATE_SCANNING: return "SCANNING";
        case STATE_CONNECTING: return "CONNECTING";
        case STATE_HANDSHAKE: return "HANDSHAKE";
        case STATE_MONITORING: return "MONITORING";
        case STATE_COOLDOWN: return "COOLDOWN";
        default: return "UNKNOWN";
    }
}

// ============================================================================
// Battery Monitor Class
// ============================================================================
class BatteryMonitor {
public:
    // Configuration
    uint8_t configIndex;
    const DeviceConfig* config;
    
    // BLE
    NimBLEClient* pClient;
    NimBLERemoteCharacteristic* pWriteChar;
    NimBLERemoteCharacteristic* pNotifyChar;
    
    // State
    DeviceState state;
    uint8_t connectRetries;
    unsigned long lastRetryTime;
    unsigned long lastNotificationTime;
    unsigned long stateEnterTime;  // When we entered current state
    NimBLEAddress deviceAddress;  // Store discovered device address
    
    // Data
    float voltage;
    uint8_t soc;
    int8_t temperature;
    uint8_t status;
    uint16_t rapidVoltageRise;  // Rapid voltage rise event counter (e.g., alternator starts)
    uint16_t rapidVoltageDrop;  // Rapid voltage drop event counter (e.g., heavy load, engine off)
    unsigned long lastUpdateTime;
    uint8_t notifyCount;  // Track notification count (skip first 5 due to invalid data)
    
    BatteryMonitor() :
        configIndex(0), config(nullptr), pClient(nullptr), 
        pWriteChar(nullptr), pNotifyChar(nullptr),
        state(STATE_DISCONNECTED), connectRetries(0), 
        lastRetryTime(0), lastNotificationTime(0), stateEnterTime(0),
        deviceAddress(NimBLEAddress("")),
        voltage(0), soc(0), temperature(0), status(0),
        rapidVoltageRise(0), rapidVoltageDrop(0),
        lastUpdateTime(0), notifyCount(0) {}
    
    void init(uint8_t index, const DeviceConfig* cfg) {
        configIndex = index;
        config = cfg;
        state = STATE_DISCONNECTED;
        connectRetries = 0;
        
        Serial.printf("[%s] Initialized: %s (Type: 0x%02X)\n", 
            config->name, config->serial, config->type);
    }
    
    String getMacAddress() {
        String mac = config->serial;
        // Convert "50547B815AFB" to "50:54:7B:81:5A:FB"
        String formatted = "";
        for (int i = 0; i < 12; i += 2) {
            if (i > 0) formatted += ":";
            formatted += mac.substring(i, i + 2);
        }
        return formatted;
    }
    
    void cleanup() {
        if (pClient) {
            if (pClient->isConnected()) {
                pClient->disconnect();
            }
            NimBLEDevice::deleteClient(pClient);
            pClient = nullptr;
        }
        pWriteChar = nullptr;
        pNotifyChar = nullptr;
        state = STATE_DISCONNECTED;
    }
    
    bool isInCooldown() {
        if (state == STATE_COOLDOWN) {
            if (millis() - lastRetryTime >= RETRY_COOLDOWN_MS) {
                connectRetries = 0;
                state = STATE_DISCONNECTED;
                return false;
            }
            return true;
        }
        return false;
    }
};

#endif // BATTERY_MONITOR_H
