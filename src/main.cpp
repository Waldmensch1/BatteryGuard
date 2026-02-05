/**
 * Battery Guard Multi-Device Monitor
 * 
 * Monitors up to 4 Battery Guard devices simultaneously via BLE
 * - Automatic device discovery and connection
 * - Encrypted handshake protocol (AES-128-CBC)
 * - Real-time monitoring of voltage, SOC, temperature, and status
 * - Automatic reconnection on disconnect
 * 
 * Hardware: ESP32
 * Libraries: NimBLE-Arduino, mbedtls
 */

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <mbedtls/aes.h>
#include "config.h"

// Debug mode controlled by platformio.ini build flags
#ifndef DEBUG_MODE
  #define DEBUG_MODE 0
#endif

#if DEBUG_MODE
  #define DEBUG_PRINT(x) Serial.print(x)
  #define DEBUG_PRINTF(...) Serial.printf(__VA_ARGS__)
  #define DEBUG_PRINTLN(x) Serial.println(x)
  #define DEBUG_TIMESTAMP() Serial.printf("[%6.2fs] ", millis()/1000.0)
#else
  #define DEBUG_PRINT(x)
  #define DEBUG_PRINTF(...)
  #define DEBUG_PRINTLN(x)
  #define DEBUG_TIMESTAMP()
#endif

// ============================================================================
// AES Initialization Vector (Zero IV)
// ============================================================================
const uint8_t AES_IV[16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// ============================================================================
// BLE UUIDs
// ============================================================================
static const NimBLEUUID SERVICE_UUID("0000fff0-0000-1000-8000-00805f9b34fb");
static const NimBLEUUID CHAR_WRITE_UUID("0000fff3-0000-1000-8000-00805f9b34fb");   // Handshake
static const NimBLEUUID CHAR_NOTIFY_UUID("0000fff4-0000-1000-8000-00805f9b34fb");  // Data

// ============================================================================
// Device State Machine
// ============================================================================
enum DeviceState {
    STATE_DISCONNECTED,     // Not connected
    STATE_SCANNING,         // Searching for device
    STATE_CONNECTING,       // Establishing connection
    STATE_HANDSHAKE,        // Sending handshake
    STATE_MONITORING,       // Receiving data
    STATE_COOLDOWN          // Waiting after failed retries
};

const char* stateToString(DeviceState state) {
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
// Device Monitor Instance
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
    
    BatteryMonitor() :
        configIndex(0), config(nullptr), pClient(nullptr), 
        pWriteChar(nullptr), pNotifyChar(nullptr),
        state(STATE_DISCONNECTED), connectRetries(0), 
        lastRetryTime(0), lastNotificationTime(0), stateEnterTime(0),
        deviceAddress(NimBLEAddress("")),
        voltage(0), soc(0), temperature(0), status(0),
        rapidVoltageRise(0), rapidVoltageDrop(0),
        lastUpdateTime(0) {}
    
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

// ============================================================================
// Global Variables
// ============================================================================
BatteryMonitor monitors[4];
uint8_t activeMonitorCount = 0;
NimBLEScan* pBLEScan;
bool scanningActive = false;

// ============================================================================
// AES Encryption/Decryption
// ============================================================================
void aes_encrypt(const uint8_t* input, uint8_t* output) {
    uint8_t iv_copy[16];
    memcpy(iv_copy, AES_IV, 16);  // Make a copy since CBC modifies IV
    
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, AES_KEY, 128);
    mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, 16, iv_copy, input, output);
    mbedtls_aes_free(&aes);
}

void aes_decrypt(const uint8_t* input, uint8_t* output) {
    uint8_t iv_copy[16];
    memcpy(iv_copy, AES_IV, 16);  // Make a copy since CBC modifies IV
    
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_dec(&aes, AES_KEY, 128);
    mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, 16, iv_copy, input, output);
    mbedtls_aes_free(&aes);
}

// ============================================================================
// Handshake Commands (Standard Mode - 6 Writes)
// ============================================================================
void sendHandshake(BatteryMonitor* monitor) {
    if (!monitor->pWriteChar) {
        Serial.printf("[%s] ERROR: Write characteristic not available\n", monitor->config->name);
        return;
    }
    
    DEBUG_TIMESTAMP();
    DEBUG_PRINTF("[%s] Starting handshake sequence (Type: 0x%02X)...\n", 
        monitor->config->name, monitor->config->type);
    
    // Define 6 handshake commands
    uint8_t commands[6][16] = {
        // Write #1: Session Init
        {0xD1, 0x55, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
        // Write #2: Battery Type
        {0xD1, 0x55, 0x08, monitor->config->type, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
        // Write #3: Config 0x1E
        {0xD1, 0x55, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
        // Write #4: Config 0xCA94
        {0xD1, 0x55, 0x05, 0x00, 0x00, 0x00, 0x00, 0xCA, 0x94, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
        // Write #5: Pre-finalization
        {0xD1, 0x55, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
        // Write #6: Finalization
        {0xD1, 0x55, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
    };
    
    // Encrypt and send each command
    for (int i = 0; i < 6; i++) {
        uint8_t encrypted[16];
        aes_encrypt(commands[i], encrypted);
        
        DEBUG_TIMESTAMP();
        DEBUG_PRINTF("[%s] Write #%d plaintext: ", monitor->config->name, i + 1);
        for (int j = 0; j < 16; j++) {
            DEBUG_PRINTF("%02X ", commands[i][j]);
        }
        DEBUG_PRINTLN("");
        
        DEBUG_TIMESTAMP();
        DEBUG_PRINTF("[%s] Write #%d encrypted: ", monitor->config->name, i + 1);
        for (int j = 0; j < 16; j++) {
            DEBUG_PRINTF("%02X ", encrypted[j]);
        }
        DEBUG_PRINTLN("");
        
        bool writeSuccess = monitor->pWriteChar->writeValue(encrypted, 16, false);
        DEBUG_TIMESTAMP();
        DEBUG_PRINTF("[%s] Write #%d result: %s\n", monitor->config->name, i + 1, writeSuccess ? "OK" : "FAILED");
        delay(50); // Small delay between writes
    }
    
    DEBUG_TIMESTAMP();
    Serial.printf("[%s] Handshake complete, waiting for notifications...\n", 
        monitor->config->name);
    
    unsigned long nowTime = millis();
    monitor->state = STATE_MONITORING;
    monitor->stateEnterTime = nowTime;
    monitor->lastNotificationTime = nowTime;
    monitor->lastUpdateTime = nowTime;
    
    DEBUG_TIMESTAMP();
    DEBUG_PRINTF("[%s] Set stateEnterTime=%lu, lastNotificationTime=%lu\n",
        monitor->config->name, monitor->stateEnterTime, monitor->lastNotificationTime);
}

// ============================================================================
// Notification Callback
// ============================================================================
void notifyCallback(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
    // Find which monitor this notification belongs to
    BatteryMonitor* monitor = nullptr;
    for (int i = 0; i < activeMonitorCount; i++) {
        if (monitors[i].pNotifyChar == pChar) {
            monitor = &monitors[i];
            break;
        }
    }
    
    if (!monitor) {
        DEBUG_TIMESTAMP();
        DEBUG_PRINTLN("Notification from unknown monitor");
        return;
    }
    
    if (length != 16) {
        DEBUG_TIMESTAMP();
        DEBUG_PRINTF("[%s] Invalid notification length: %d\n", monitor->config->name, length);
        return;
    }
    
    DEBUG_TIMESTAMP();
    DEBUG_PRINTF("[%s] Notification received (%d bytes) - RAW: ", monitor->config->name, length);
    for (int i = 0; i < length; i++) {
        DEBUG_PRINTF("%02X ", pData[i]);
    }
    DEBUG_PRINTLN("");
    
    // Decrypt notification
    uint8_t decrypted[16];
    aes_decrypt(pData, decrypted);
    
    DEBUG_TIMESTAMP();
    DEBUG_PRINTF("[%s] Decrypted: ", monitor->config->name);
    for (int i = 0; i < 16; i++) {
        DEBUG_PRINTF("%02X ", decrypted[i]);
    }
    DEBUG_PRINTLN("");
    
    // Verify header
    if (decrypted[0] != 0xD1 || decrypted[1] != 0x55) {
        DEBUG_TIMESTAMP();
        DEBUG_PRINTF("[%s] Invalid header: %02X %02X\n", monitor->config->name, decrypted[0], decrypted[1]);
        return;
    }
    
    // Parse data (based on Android app analysis)
    // Byte 3: Temperature sign (1 = negative)
    // Byte 4: Temperature value
    if (decrypted[3] == 1) {
        monitor->temperature = -(int8_t)decrypted[4];
    } else {
        monitor->temperature = (int8_t)decrypted[4];
    }
    
    monitor->status = decrypted[5];
    monitor->soc = decrypted[6];
    monitor->voltage = ((decrypted[7] << 8) | decrypted[8]) / 100.0f;
    monitor->rapidVoltageRise = (decrypted[9] << 8) | decrypted[10];
    monitor->rapidVoltageDrop = (decrypted[11] << 8) | decrypted[12];
    monitor->lastUpdateTime = millis();
    monitor->lastNotificationTime = millis();
    
    // Status string (from Android app analysis)
    const char* statusStr = "unknown";
    switch (monitor->status) {
        case 0x00: statusStr = "normal"; break;
        case 0x01: statusStr = "low"; break;
        case 0x02: statusStr = "engine off"; break;
        case 0x03: statusStr = "charging"; break;
        default: statusStr = "unknown"; break;
    }
    
    // Log output with extended data
    Serial.printf("%s (%s): %.2fV | %d%% | %d°C | %s | VRise:%d | VDrop:%d\n",
        monitor->config->name, monitor->config->serial,
        monitor->voltage, monitor->soc, monitor->temperature, statusStr,
        monitor->rapidVoltageRise, monitor->rapidVoltageDrop);
}

// ============================================================================
// Client Callbacks
// ============================================================================
class ClientCallbacks : public NimBLEClientCallbacks {
    BatteryMonitor* monitor;
    
public:
    ClientCallbacks(BatteryMonitor* mon) : monitor(mon) {}
    
    void onConnect(NimBLEClient* pClient) {
        DEBUG_PRINTF("[%s] CALLBACK: onConnect fired\n", monitor->config->name);
        Serial.printf("[%s] Connected!\n", monitor->config->name);
    }
    
    void onDisconnect(NimBLEClient* pClient) {
        DEBUG_PRINTF("[%s] CALLBACK: onDisconnect fired (reason: %d)\n", 
            monitor->config->name, pClient->getLastError());
        Serial.printf("[%s] Disconnected\n", monitor->config->name);
        monitor->state = STATE_DISCONNECTED;
        monitor->pWriteChar = nullptr;
        monitor->pNotifyChar = nullptr;
    }
};

// ============================================================================
// Connection Management
// ============================================================================
bool connectToDevice(BatteryMonitor* monitor, NimBLEAdvertisedDevice* device) {
    if (monitor->state == STATE_COOLDOWN) return false;
    
    monitor->state = STATE_CONNECTING;
    Serial.printf("[%s] Connecting to %s...\n", 
        monitor->config->name, monitor->getMacAddress().c_str());
    
    // Stop scanning before connecting - critical for ESP32
    if (NimBLEDevice::getScan()->isScanning()) {
        DEBUG_TIMESTAMP();
        DEBUG_PRINTLN("Stopping scan before connection attempt");
        NimBLEDevice::getScan()->stop();
        delay(100); // Give time for scan to fully stop
    }
    
    // Create client if needed
    if (!monitor->pClient) {
        DEBUG_TIMESTAMP();
        DEBUG_PRINTF("[%s] Creating new BLE client\n", monitor->config->name);
        monitor->pClient = NimBLEDevice::createClient();
        monitor->pClient->setClientCallbacks(new ClientCallbacks(monitor), false);
        DEBUG_TIMESTAMP();
        DEBUG_PRINTF("[%s] Client created, callbacks set\n", monitor->config->name);
    }
    
    // Get MAC address from device
    NimBLEAddress deviceAddress = device->getAddress();
    DEBUG_TIMESTAMP();
    DEBUG_PRINTF("[%s] Attempting connection to %s (Attempt %d/%d)...\n", 
        monitor->config->name, deviceAddress.toString().c_str(),
        monitor->connectRetries + 1, MAX_CONNECT_RETRIES);
    
    // Connect using MAC address (not device pointer) - this is more reliable
    // Use default timeout (30 seconds) - Bridge solution takes ~30s to connect
    
    DEBUG_TIMESTAMP();
    DEBUG_PRINTF("[%s] Starting connection (using default 30s timeout)...\n", monitor->config->name);
    
    unsigned long startTime = millis();
    bool connected = monitor->pClient->connect(deviceAddress);
    unsigned long connectTime = millis() - startTime;
    
    DEBUG_TIMESTAMP();
    DEBUG_PRINTF("[%s] connect() returned: %s (took %lums)\n", 
        monitor->config->name, connected ? "true" : "false", connectTime);
    
    if (!connected) {
        int lastError = monitor->pClient->getLastError();
        DEBUG_TIMESTAMP();
        DEBUG_PRINTF("[%s] BLE Error Code: %d\n", monitor->config->name, lastError);
        
        Serial.printf("[%s] Connection failed (Attempt %d/%d)\n", 
            monitor->config->name, monitor->connectRetries + 1, MAX_CONNECT_RETRIES);
        
        // Delete and recreate client for next attempt - clears any stale state
        if (monitor->pClient) {
            DEBUG_TIMESTAMP();
            DEBUG_PRINTLN("Deleting client to clear state");
            NimBLEDevice::deleteClient(monitor->pClient);
            monitor->pClient = nullptr;
        }
        
        monitor->connectRetries++;
        
        if (monitor->connectRetries >= MAX_CONNECT_RETRIES) {
            Serial.printf("[%s] Max retries reached, entering cooldown (30s)\n", 
                monitor->config->name);
            Serial.println("[HINT] Make sure Battery Guard app is closed on your phone!");
            monitor->state = STATE_COOLDOWN;
            monitor->lastRetryTime = millis();
        } else {
            monitor->state = STATE_DISCONNECTED;
        }
        return false;
    }
    
    DEBUG_TIMESTAMP();
    DEBUG_PRINTF("[%s] Connection established! Getting service...\n", monitor->config->name);
    
    // Get service
    NimBLERemoteService* pService = monitor->pClient->getService(SERVICE_UUID);
    if (!pService) {
        Serial.printf("[%s] ERROR: Service %s not found\n", 
            monitor->config->name, SERVICE_UUID.toString().c_str());
        monitor->pClient->disconnect();
        return false;
    }
    DEBUG_TIMESTAMP();
    DEBUG_PRINTF("[%s] Service found!\n", monitor->config->name);
    
    // Get characteristics
    DEBUG_TIMESTAMP();
    DEBUG_PRINTF("[%s] Getting characteristics...\n", monitor->config->name);
    monitor->pWriteChar = pService->getCharacteristic(CHAR_WRITE_UUID);
    monitor->pNotifyChar = pService->getCharacteristic(CHAR_NOTIFY_UUID);
    
    if (!monitor->pWriteChar || !monitor->pNotifyChar) {
        Serial.printf("[%s] ERROR: Characteristics not found (Write: %s, Notify: %s)\n", 
            monitor->config->name, 
            monitor->pWriteChar ? "OK" : "FAIL",
            monitor->pNotifyChar ? "OK" : "FAIL");
        monitor->pClient->disconnect();
        return false;
    }
    DEBUG_TIMESTAMP();
    DEBUG_PRINTF("[%s] Characteristics found!\n", monitor->config->name);
    
    // Subscribe to notifications
    DEBUG_TIMESTAMP();
    DEBUG_PRINTF("[%s] Subscribing to notifications...\n", monitor->config->name);
    if (monitor->pNotifyChar->canNotify()) {
        if (!monitor->pNotifyChar->subscribe(true, notifyCallback)) {
            Serial.printf("[%s] ERROR: Failed to subscribe to notifications\n", monitor->config->name);
            monitor->pClient->disconnect();
            return false;
        }
        DEBUG_TIMESTAMP();
        DEBUG_PRINTF("[%s] Notification subscription successful!\n", monitor->config->name);
    } else {
        Serial.printf("[%s] ERROR: Characteristic cannot notify\n", monitor->config->name);
        monitor->pClient->disconnect();
        return false;
    }
    
    // Success - reset retry counter
    monitor->connectRetries = 0;
    monitor->state = STATE_HANDSHAKE;
    DEBUG_TIMESTAMP();
    DEBUG_PRINTF("[%s] State -> HANDSHAKE\n", monitor->config->name);
    
    // Send handshake
    delay(100);
    DEBUG_TIMESTAMP();
    DEBUG_PRINTF("[%s] Sending handshake sequence...\n", monitor->config->name);
    sendHandshake(monitor);
    
    return true;
}

// ============================================================================
// Scan Callbacks
// ============================================================================
class ScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* device) {
        DEBUG_PRINTF("Scanned device: %s", device->getAddress().toString().c_str());
        if (device->haveName()) {
            DEBUG_PRINTF(" | %s", device->getName().c_str());
        }
        DEBUG_PRINTLN("");
        
        // Check if this is a Battery Guard device
        if (!device->haveName() || device->getName() != "Battery Guard") {
            return;
        }
        
        DEBUG_PRINTF("Found Battery Guard: %s\n", device->getAddress().toString().c_str());
        
        // Get MAC address without colons
        String mac = device->getAddress().toString().c_str();
        mac.replace(":", "");
        mac.toUpperCase();
        
        DEBUG_PRINTF("MAC without colons: %s\n", mac.c_str());
        
        // Check if this device is in our configuration
        for (int i = 0; i < activeMonitorCount; i++) {
            BatteryMonitor* monitor = &monitors[i];
            
            String configSerial = monitor->config->serial;
            configSerial.toUpperCase();
            
            // Check if MAC matches first
            if (mac != configSerial) continue;
            
            // MAC matches - now check if we can connect
            DEBUG_PRINTF("MAC match! State: %s, enabled: %d, connected: %d\n", 
                stateToString(monitor->state), monitor->config->enabled, 
                (monitor->pClient && monitor->pClient->isConnected()) ? 1 : 0);
            
            if (!monitor->config->enabled) {
                DEBUG_PRINTF("Skipping: not enabled\n");
                continue;
            }
            if (monitor->state == STATE_COOLDOWN) {
                DEBUG_PRINTF("Skipping: in cooldown\n");
                continue;
            }
            if (monitor->state == STATE_CONNECTING || monitor->state == STATE_MONITORING || monitor->state == STATE_HANDSHAKE) {
                DEBUG_PRINTF("Skipping: busy (state=%s)\n", stateToString(monitor->state));
                continue;
            }
            if (monitor->pClient && monitor->pClient->isConnected()) {
                DEBUG_PRINTF("Skipping: already connected\n");
                continue;
            }
            
            DEBUG_PRINTF("Comparing MAC '%s' with config '%s'\n", mac.c_str(), configSerial.c_str());
            
            if (mac == configSerial) {
                Serial.printf("[%s] Found device: %s - STOPPING SCAN!\n", 
                    monitor->config->name, device->getAddress().toString().c_str());
                
                // Stop scanning immediately in callback
                NimBLEDevice::getScan()->stop();
                delay(100);
                
                // Mark as ready to connect and store address
                monitor->state = STATE_SCANNING;
                monitor->deviceAddress = device->getAddress();
                return;
            }
        }
        
        DEBUG_PRINTF("Device not in config list\n");
    }
};

// ============================================================================
// Setup
// ============================================================================
void setup() {
    Serial.begin(115200);
    while (!Serial) delay(10);
    
    Serial.println("\n\n============================================================");
    Serial.println("Battery Guard Multi-Device Monitor");
    Serial.println("============================================================");
    
    // Initialize BLE
    NimBLEDevice::init("ESP32-Monitor");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    
    // Setup scan
    pBLEScan = NimBLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(new ScanCallbacks(), false);
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(SCAN_INTERVAL);
    pBLEScan->setWindow(SCAN_WINDOW);
    
    // Initialize monitors
    for (int i = 0; i < DEVICE_COUNT && i < 4; i++) {
        if (DEVICES[i].enabled) {
            monitors[activeMonitorCount].init(i, &DEVICES[i]);
            activeMonitorCount++;
        }
    }
    
    Serial.printf("\nMonitoring %d device(s):\n", activeMonitorCount);
    for (int i = 0; i < activeMonitorCount; i++) {
        Serial.printf("  [%d] %s (%s) - Type: 0x%02X\n", 
            i + 1, monitors[i].config->name, monitors[i].config->serial, 
            monitors[i].config->type);
    }
    
    Serial.println("============================================================\n");
    
    // Start scanning
    pBLEScan->start(0, false);
    scanningActive = true;
}

// ============================================================================
// Loop
// ============================================================================
void loop() {
    // Check monitor states first
    unsigned long now = millis();
    bool needToConnect = false;
    
    // Debug: Show all monitor states every 5 seconds
    static unsigned long lastStateDebug = 0;
    if (now - lastStateDebug > 5000) {
        DEBUG_TIMESTAMP();
        DEBUG_PRINT("Monitor states: ");
        for (int i = 0; i < activeMonitorCount; i++) {
            DEBUG_PRINTF("[%d:%s] ", i, stateToString(monitors[i].state));
        }
        DEBUG_PRINTF("| needToConnect=%d scanningActive=%d isScanning=%d\n", 
            needToConnect, scanningActive, pBLEScan->isScanning());
        lastStateDebug = now;
    }
    
    for (int i = 0; i < activeMonitorCount; i++) {
        BatteryMonitor* monitor = &monitors[i];
        
        // Debug: Print state if it's SCANNING
        if (monitor->state == STATE_SCANNING) {
            DEBUG_TIMESTAMP();
            DEBUG_PRINTF("[%s] LOOP: Detected STATE_SCANNING! Initiating connection...\n", monitor->config->name);
        }
        
        // Handle device ready to connect
        if (monitor->state == STATE_SCANNING && !needToConnect) {
            // Stop scanning and connect
            if (pBLEScan->isScanning()) {
                DEBUG_TIMESTAMP();
                DEBUG_PRINTLN("Stopping scan to connect to device");
                pBLEScan->stop();
                scanningActive = false;
                delay(100);  // Give BLE stack time to stop
            }
            
            // Now connect (refactored to not need device pointer)
            monitor->state = STATE_CONNECTING;
            Serial.printf("[%s] Connecting to %s...\n", 
                monitor->config->name, monitor->deviceAddress.toString().c_str());
            
            // Create client
            monitor->pClient = NimBLEDevice::createClient();
            monitor->pClient->setClientCallbacks(new ClientCallbacks(monitor), false);
            
            DEBUG_TIMESTAMP();
            DEBUG_PRINTF("[%s] Attempting connection (Attempt %d/%d)...\n", 
                monitor->config->name, monitor->connectRetries + 1, MAX_CONNECT_RETRIES);
            
            unsigned long startTime = millis();
            bool connected = monitor->pClient->connect(monitor->deviceAddress);
            unsigned long connectTime = millis() - startTime;
            
            DEBUG_TIMESTAMP();
            DEBUG_PRINTF("[%s] connect() returned: %s (took %lums)\n", 
                monitor->config->name, connected ? "true" : "false", connectTime);
            
            if (!connected) {
                Serial.printf("[%s] Connection failed (Attempt %d/%d)\n", 
                    monitor->config->name, monitor->connectRetries + 1, MAX_CONNECT_RETRIES);
                
                NimBLEDevice::deleteClient(monitor->pClient);
                monitor->pClient = nullptr;
                monitor->connectRetries++;
                
                if (monitor->connectRetries >= MAX_CONNECT_RETRIES) {
                    Serial.printf("[%s] Max retries reached, entering cooldown\n", monitor->config->name);
                    monitor->state = STATE_COOLDOWN;
                    monitor->lastRetryTime = now;
                } else {
                    DEBUG_TIMESTAMP();
                    DEBUG_PRINTF("[%s] Setting state to DISCONNECTED for retry\n", monitor->config->name);
                    monitor->state = STATE_DISCONNECTED;
                }
            } else {
                // Connected! Now get service and characteristics
                DEBUG_TIMESTAMP();
                DEBUG_PRINTF("[%s] Connection successful! Discovering services...\n", monitor->config->name);
                
                NimBLERemoteService* pService = monitor->pClient->getService(SERVICE_UUID);
                if (!pService) {
                    Serial.printf("[%s] ERROR: Service not found\n", monitor->config->name);
                    monitor->pClient->disconnect();
                    monitor->state = STATE_DISCONNECTED;
                    continue;
                }
                
                monitor->pWriteChar = pService->getCharacteristic(CHAR_WRITE_UUID);
                monitor->pNotifyChar = pService->getCharacteristic(CHAR_NOTIFY_UUID);
                
                if (!monitor->pWriteChar || !monitor->pNotifyChar) {
                    Serial.printf("[%s] ERROR: Characteristics not found\n", monitor->config->name);
                    monitor->pClient->disconnect();
                    monitor->state = STATE_DISCONNECTED;
                    continue;
                }
                
                // Subscribe to notifications
                if (monitor->pNotifyChar->canNotify()) {
                    if (!monitor->pNotifyChar->subscribe(true, notifyCallback)) {
                        Serial.printf("[%s] ERROR: Failed to subscribe\n", monitor->config->name);
                        monitor->pClient->disconnect();
                        monitor->state = STATE_DISCONNECTED;
                        continue;
                    }
                }
                
                // Success! Send handshake
                Serial.printf("[%s] Connected successfully!\n", monitor->config->name);
                monitor->connectRetries = 0;
                monitor->state = STATE_HANDSHAKE;
                delay(100);
                sendHandshake(monitor);
            }
            
            needToConnect = true;  // Don't try multiple connections in one loop
        }
        
        // Check cooldown state
        if (monitor->state == STATE_COOLDOWN) {
            if (now - monitor->lastRetryTime >= RETRY_COOLDOWN_MS) {
                Serial.printf("[%s] Cooldown expired, resuming scan\n", monitor->config->name);
                monitor->state = STATE_DISCONNECTED;
                monitor->connectRetries = 0;
            }
        }
        
        // Check notification timeout (only after grace period)
        if (monitor->state == STATE_MONITORING) {
            unsigned long currentTime = millis();  // Get fresh time
            unsigned long timeInState = currentTime - monitor->stateEnterTime;
            if (timeInState > 2000) {  // Grace period: 2 seconds after entering MONITORING
                unsigned long timeSinceNotif = currentTime - monitor->lastNotificationTime;
                if (timeSinceNotif > NOTIFICATION_TIMEOUT_MS) {
                    DEBUG_TIMESTAMP();
                    DEBUG_PRINTF("[%s] Notification timeout: now=%lu, lastNotif=%lu, diff=%lums (threshold: %lu)\n", 
                        monitor->config->name, currentTime, monitor->lastNotificationTime, timeSinceNotif, NOTIFICATION_TIMEOUT_MS);
                    Serial.printf("[%s] Notification timeout, disconnecting\n", monitor->config->name);
                    monitor->cleanup();
                }
            }
        }
    }
    
    // Keep scanningActive flag in sync with actual scan state
    if (pBLEScan->isScanning() && !scanningActive) {
        scanningActive = true;
    } else if (!pBLEScan->isScanning() && scanningActive) {
        scanningActive = false;
    }
    
    // Restart scan if not active and not connecting
    if (!needToConnect && !scanningActive) {
        DEBUG_TIMESTAMP();
        DEBUG_PRINTLN("Restarting scan...");
        pBLEScan->start(0, false);
        scanningActive = true;
    }
    
    delay(100);
}