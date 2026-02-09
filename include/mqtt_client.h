#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#ifdef MQTT_ENABLED

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "config.h"
#include "battery_monitor.h"

// Constants
#define MAX_DEVICES 4

// MQTT Client class for Battery Guard monitoring
class MQTTClient {
public:
    MQTTClient();
    
    // Initialize WiFi and MQTT connection
    bool begin();
    
    // Main loop - call regularly to maintain connection and process messages
    void loop();
    
    // Publish battery data for a specific monitor
    void publishBatteryData(const BatteryMonitor* monitor);
    
    // Check if MQTT is connected
    bool isConnected();
    
private:
    WiFiClient wifiClient;
    PubSubClient mqttClient;
    unsigned long lastReconnectAttempt;
    unsigned long lastPublishTime[MAX_DEVICES];
    
    // Connection management
    bool connectWiFi();
    bool connectMQTT();
    void reconnect();
    
    // Publishing
    void publishHomeAssistantDiscovery(const DeviceConfig* config);
    void publishState(const BatteryMonitor* monitor);
    String buildStateTopic(const char* mqttName);
    String buildDiscoveryTopic(const char* mqttName, const char* sensor);
    String buildJsonPayload(const BatteryMonitor* monitor);
    String buildHomeAssistantConfig(const DeviceConfig* config, const char* sensor, const char* unit, const char* deviceClass);
};

extern MQTTClient mqttClient;

#endif // MQTT_ENABLED

#endif // MQTT_CLIENT_H // MQTT_CLIENT_H
