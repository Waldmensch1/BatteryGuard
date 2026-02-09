#include "tft_display.h"

#ifdef LCD_ENABLED

// External reference to shared data array (defined in main.cpp)
extern DeviceDisplayData g_displayData[MAX_MONITORS];

// TFT display object
static TFT_eSPI tft = TFT_eSPI();

// Display task handle
static TaskHandle_t displayTaskHandle = NULL;

// Display configuration
#define DISPLAY_UPDATE_INTERVAL_MS 2000  // Update every 2 seconds
#define DEVICE_HEIGHT 40                  // Pixels per device row



// Initialize display hardware
void initDisplay() {
    tft.init();
    tft.setRotation(0);  // Portrait mode (128x160)
    tft.fillScreen(TFT_BLACK);
    
    // Draw header (like in lcd_test)
    tft.fillRect(0, 0, 128, 22, TFT_NAVY);
    tft.setTextColor(TFT_WHITE, TFT_NAVY);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("Bat Monitor", 64, 11, 2);
}

// Draw single device data
void drawDevice(int index, int yPos) {
    DeviceDisplayData data = g_displayData[index];
    
    // Local cache to detect changes
    static float lastVoltage[MAX_MONITORS] = {0};
    static int lastSoc[MAX_MONITORS] = {0};
    static int lastTemp[MAX_MONITORS] = {0};
    static uint8_t lastStatus[MAX_MONITORS] = {0xFF};
    static bool lastConnected[MAX_MONITORS] = {false};
    static bool initialized[MAX_MONITORS] = {false};
    
    if (!data.active) {
        return;
    }
    
    // Force full redraw on first call or connection change
    bool forceRedraw = !initialized[index] || (lastConnected[index] != data.connected);
    
    // Update header with device name
    tft.fillRect(0, 0, 128, 22, TFT_NAVY);
    tft.setTextColor(TFT_WHITE, TFT_NAVY);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(data.name, 64, 11, 2);
    
    if (!data.connected) {
        // Show disconnected message
        tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
        tft.setTextDatum(MC_DATUM);
        tft.drawString("Disconnected", 64, 80, 2);
        lastConnected[index] = false;
        return;
    }
    
    lastConnected[index] = true;
    
    // Voltage - Large display (only if changed)
    if (forceRedraw || lastVoltage[index] != data.voltage) {
        // Clear old text area
        tft.fillRect(20, 30, 88, 30, TFT_BLACK);
        tft.setTextColor(TFT_GREEN, TFT_BLACK);
        tft.setTextDatum(MC_DATUM);
        char voltStr[16];
        sprintf(voltStr, "%.2f V", data.voltage);
        tft.drawString(voltStr, 64, 45, 4);
        lastVoltage[index] = data.voltage;
    }
    
    // SOC Progress Bar (only if changed)
    if (forceRedraw || lastSoc[index] != data.soc) {
        int barY = 70;
        int barHeight = 12;
        int barWidth = 110;
        int barX = (128 - barWidth) / 2;
        int fillWidth = (barWidth - 2) * data.soc / 100;
        
        tft.drawRect(barX, barY, barWidth, barHeight, TFT_WHITE);
        tft.fillRect(barX + 1, barY + 1, barWidth - 2, barHeight - 2, TFT_BLACK);
        tft.fillRect(barX + 1, barY + 1, fillWidth, barHeight - 2, TFT_GREEN);
        
        // SOC percentage - clear area first
        tft.fillRect(40, 85, 48, 20, TFT_BLACK);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.setTextDatum(MC_DATUM);
        char socStr[16];
        sprintf(socStr, "%d%%", data.soc);
        tft.drawString(socStr, 64, 92, 2);
        lastSoc[index] = data.soc;
    }
    
    // Temperature - centered, 5 pixels lower (only if changed)
    if (forceRedraw || lastTemp[index] != data.temperature) {
        // Clear entire line to prevent pixel artifacts when text width changes
        tft.fillRect(0, 105, 128, 20, TFT_BLACK);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.setTextDatum(MC_DATUM);
        char tempStr[20];
        sprintf(tempStr, "Temperature %d C", data.temperature);
        tft.drawString(tempStr, 64, 115, 2);
        lastTemp[index] = data.temperature;
    }
    
    // Status - centered, another 5 pixels lower (only if changed)
    if (forceRedraw || lastStatus[index] != data.status) {
        tft.fillRect(20, 125, 88, 20, TFT_BLACK);
        tft.setTextColor(TFT_YELLOW, TFT_BLACK);
        tft.drawString(getBatteryStatusText(data.status), 64, 135, 2);
        lastStatus[index] = data.status;
    }
    
    initialized[index] = true;
}

// Display task - runs on Core 0
void displayTask(void* parameter) {
    Serial.println("[DISPLAY] Task started on Core 0");
    
    // Initialize display on Core 0 (thread-safe)
    Serial.println("[DISPLAY] Initializing TFT on Core 0...");
    tft.init();
    tft.setRotation(0);
    tft.fillScreen(TFT_BLACK);
    
    // Show startup screen - 2 lines
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("Battery", 64, 60, 4);
    tft.drawString("Guard", 64, 85, 4);
    tft.drawString("Connecting....", 64, 120, 2);
    Serial.println("[DISPLAY] TFT initialized");
    
    unsigned long lastUpdate = 0;
    unsigned long lastDeviceSwitch = 0;
    int currentDeviceIndex = 0;
    bool startupShown = true;
    
    while (true) {
        unsigned long now = millis();
        
        // Update display every DISPLAY_UPDATE_INTERVAL_MS
        if (now - lastUpdate >= DISPLAY_UPDATE_INTERVAL_MS) {
            lastUpdate = now;
            
            // Check for active connected devices
            bool hasData = false;
            int connectedCount = 0;
            int connectedDevices[MAX_MONITORS];
            
            for (int i = 0; i < MAX_MONITORS; i++) {
                if (g_displayData[i].active && g_displayData[i].connected) {
                    connectedDevices[connectedCount++] = i;
                    hasData = true;
                }
            }
            
            // Switch device every 15 seconds if multiple connected
            if (connectedCount > 1 && (now - lastDeviceSwitch >= 15000)) {
                lastDeviceSwitch = now;
                currentDeviceIndex = (currentDeviceIndex + 1) % connectedCount;
            }
            
            // Reset device index if only one or none connected
            if (connectedCount <= 1) {
                currentDeviceIndex = 0;
                lastDeviceSwitch = now;
            }
            
            // Clear screen when transitioning from startup to data
            if (startupShown && hasData) {
                tft.fillScreen(TFT_BLACK);
                startupShown = false;
            }
            
            if (!hasData && !startupShown) {
                // Back to startup screen
                tft.fillScreen(TFT_BLACK);
                tft.setTextColor(TFT_WHITE, TFT_BLACK);
                tft.setTextDatum(MC_DATUM);
                tft.drawString("Battery", 64, 60, 4);
                tft.drawString("Guard", 64, 85, 4);
                tft.drawString("Connecting....", 64, 120, 2);
                startupShown = true;
            }
            
            if (hasData) {
                // Draw currently selected connected device
                // No longer clear the entire area - drawDevice handles selective updates
                if (connectedCount > 0) {
                    drawDevice(connectedDevices[currentDeviceIndex], 30);
                }
            }
        }
        
        // Yield to other tasks
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// Start the display task on Core 0
void startDisplayTask() {
    xTaskCreatePinnedToCore(
        displayTask,           // Task function
        "DisplayTask",         // Task name
        4096,                  // Stack size (bytes)
        NULL,                  // Parameter
        1,                     // Priority
        &displayTaskHandle,    // Task handle
        0                      // Core 0
    );
    
    Serial.println("[DISPLAY] Task created on Core 0");
}

#endif // LCD_ENABLED
