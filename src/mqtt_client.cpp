#ifdef MQTT_ENABLED

#include "mqtt_client.h"
#include <ArduinoJson.h>
#include <time.h>

// Global instance
MQTTClient mqttClient;

// Constructor
MQTTClient::MQTTClient() : 
    mqttClient(wifiClient),
    lastReconnectAttempt(0) {
    for (int i = 0; i < MAX_DEVICES; i++) {
        lastPublishTime[i] = 0;
    }
}

// Initialize WiFi and MQTT
bool MQTTClient::begin() {
    #ifdef DEBUG_MODE
    Serial.println("[MQTT] Initializing...");
    #endif
    
    if (!connectWiFi()) {
        return false;
    }
    
    // Configure MQTT server
    mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
    mqttClient.setBufferSize(1024);  // Increase buffer for Home Assistant discovery
    
    return connectMQTT();
}

// Connect to WiFi
bool MQTTClient::connectWiFi() {
    #ifdef DEBUG_MODE
    Serial.printf("[MQTT] Connecting to WiFi: %s\n", WIFI_SSID);
    #endif
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        #ifdef DEBUG_MODE
        Serial.print(".");
        #endif
        attempts++;
    }
    
    if (WiFi.status() != WL_CONNECTED) {
        #ifdef DEBUG_MODE
        Serial.println("\n[MQTT] WiFi connection failed!");
        #endif
        return false;
    }
    
    #ifdef DEBUG_MODE
    Serial.printf("\n[MQTT] WiFi connected! IP: %s\n", WiFi.localIP().toString().c_str());
    #endif
    
    // Configure NTP time synchronization (GMT+1, DST+1)
    configTime(3600, 3600, "pool.ntp.org", "time.nist.gov");
    #ifdef DEBUG_MODE
    Serial.println("[MQTT] NTP time sync started");
    #endif
    
    return true;
}

// Connect to MQTT broker
bool MQTTClient::connectMQTT() {
    #ifdef DEBUG_MODE
    Serial.printf("[MQTT] Connecting to broker: %s:%d\n", MQTT_SERVER, MQTT_PORT);
    #endif
    
    String clientId = "BatteryGuard-";
    clientId += String((uint32_t)ESP.getEfuseMac(), HEX);
    
    #ifdef MQTT_USERNAME
    bool connected = mqttClient.connect(clientId.c_str(), MQTT_USERNAME, MQTT_PASSWORD);
    #else
    bool connected = mqttClient.connect(clientId.c_str());
    #endif
    
    if (connected) {
        #ifdef DEBUG_MODE
        Serial.println("[MQTT] Connected to broker!");
        #endif
        return true;
    } else {
        #ifdef DEBUG_MODE
        Serial.printf("[MQTT] Connection failed, rc=%d\n", mqttClient.state());
        #endif
        return false;
    }
}

// Reconnection logic
void MQTTClient::reconnect() {
    unsigned long now = millis();
    
    // Try reconnecting every 5 seconds
    if (now - lastReconnectAttempt > 5000) {
        lastReconnectAttempt = now;
        
        // Check WiFi first
        if (WiFi.status() != WL_CONNECTED) {
            #ifdef DEBUG_MODE
            Serial.println("[MQTT] WiFi disconnected, reconnecting...");
            #endif
            connectWiFi();
        }
        
        // Try MQTT reconnect
        if (WiFi.status() == WL_CONNECTED && !mqttClient.connected()) {
            #ifdef DEBUG_MODE
            Serial.println("[MQTT] Reconnecting to broker...");
            #endif
            connectMQTT();
        }
    }
}

// Main loop
void MQTTClient::loop() {
    if (mqttClient.connected()) {
        mqttClient.loop();
    } else {
        reconnect();
    }
}

// Check connection status
bool MQTTClient::isConnected() {
    return mqttClient.connected();
}

// Build state topic
String MQTTClient::buildStateTopic(const char* mqttName) {
    String topic = MQTT_PREFIX;
    topic += "/batteryguard/";
    topic += mqttName;
    return topic;
}

// Build Home Assistant discovery topic
String MQTTClient::buildDiscoveryTopic(const char* mqttName, const char* sensor) {
    String topic = "homeassistant/sensor/batteryguard_";
    topic += mqttName;
    topic += "_";
    topic += sensor;
    topic += "/config";
    return topic;
}

// Build JSON payload
String MQTTClient::buildJsonPayload(const BatteryMonitor* monitor) {
    StaticJsonDocument<256> doc;
    
    doc["voltage"] = round(monitor->voltage * 100.0) / 100.0;  // Round to 2 decimals
    doc["soc"] = monitor->soc;
    doc["temperature"] = monitor->temperature;
    
    // Use MQTT-specific status (without "Charge:" prefix)
    doc["charge"] = getBatteryStatusMqtt(monitor->status);
    
    // Add timestamp from NTP
    time_t now;
    time(&now);
    char timestamp[25];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
    doc["timestamp"] = timestamp;
    
    String output;
    serializeJson(doc, output);
    return output;
}

// Build Home Assistant configuration payload
String MQTTClient::buildHomeAssistantConfig(const DeviceConfig* config, const char* sensor, const char* unit, const char* deviceClass) {
    StaticJsonDocument<768> doc;
    
    String objectId = "batteryguard_";
    objectId += config->mqttName;
    objectId += "_";
    objectId += sensor;
    
    doc["name"] = String(config->name) + " " + String(sensor);
    doc["unique_id"] = objectId;
    doc["state_topic"] = buildStateTopic(config->mqttName);
    doc["value_template"] = String("{{ value_json.") + sensor + " }}";
    
    if (unit && strlen(unit) > 0) {
        doc["unit_of_measurement"] = unit;
    }
    
    if (deviceClass && strlen(deviceClass) > 0) {
        doc["device_class"] = deviceClass;
    }
    
    // Add timestamp sensor availability
    if (strcmp(sensor, "timestamp") != 0) {
        doc["json_attributes_topic"] = buildStateTopic(config->mqttName);
        doc["json_attributes_template"] = "{{ {'timestamp': value_json.timestamp} | tojson }}";
    }
    
    // Device info
    JsonObject device = doc.createNestedObject("device");
    device["identifiers"][0] = String("batteryguard_") + config->mqttName;
    device["name"] = config->name;
    device["manufacturer"] = "Battery Guard";
    device["model"] = "BLE Monitor";
    
    String output;
    serializeJson(doc, output);
    return output;
}

// Publish Home Assistant discovery messages
void MQTTClient::publishHomeAssistantDiscovery(const DeviceConfig* config) {
    #ifdef HOMEASSIST_FORMAT
    if (!mqttClient.connected()) return;
    
    #ifdef DEBUG_MODE
    Serial.printf("[MQTT] Publishing Home Assistant discovery for %s\n", config->name);
    #endif
    
    // Voltage sensor
    String topic = buildDiscoveryTopic(config->mqttName, "voltage");
    String payload = buildHomeAssistantConfig(config, "voltage", "V", "voltage");
    mqttClient.publish(topic.c_str(), payload.c_str(), MQTT_RETAINED);
    
    delay(50);  // Small delay between discovery messages
    
    // SOC sensor
    topic = buildDiscoveryTopic(config->mqttName, "soc");
    payload = buildHomeAssistantConfig(config, "soc", "%", "battery");
    mqttClient.publish(topic.c_str(), payload.c_str(), MQTT_RETAINED);
    
    delay(50);
    
    // Temperature sensor
    topic = buildDiscoveryTopic(config->mqttName, "temperature");
    payload = buildHomeAssistantConfig(config, "temperature", "°C", "temperature");
    mqttClient.publish(topic.c_str(), payload.c_str(), MQTT_RETAINED);
    
    delay(50);
    
    // Charge status sensor
    topic = buildDiscoveryTopic(config->mqttName, "charge");
    payload = buildHomeAssistantConfig(config, "charge", "", "");
    mqttClient.publish(topic.c_str(), payload.c_str(), MQTT_RETAINED);
    
    delay(50);
    
    // Timestamp sensor
    topic = buildDiscoveryTopic(config->mqttName, "timestamp");
    payload = buildHomeAssistantConfig(config, "timestamp", "", "timestamp");
    mqttClient.publish(topic.c_str(), payload.c_str(), MQTT_RETAINED);
    
    #ifdef DEBUG_MODE
    Serial.println("[MQTT] Discovery published");
    #endif
    #endif
}

// Publish battery state
void MQTTClient::publishState(const BatteryMonitor* monitor) {
    if (!mqttClient.connected()) {
        Serial.println("[MQTT] Not connected, skipping publish");
        return;
    }
    
    unsigned long now = millis();
    int index = monitor->configIndex;
    
    // Check if enough time has passed since last publish (allow first publish immediately)
    if (lastPublishTime[index] != 0 && (now - lastPublishTime[index] < (MQTT_UPDATE_INTERVAL * 1000))) {
        return;
    }
    
    lastPublishTime[index] = now;
    
    String topic = buildStateTopic(monitor->config->mqttName);
    String payload = buildJsonPayload(monitor);
    
    Serial.printf("[MQTT] Publishing to %s: %s\n", topic.c_str(), payload.c_str());
    bool published = mqttClient.publish(topic.c_str(), payload.c_str(), MQTT_RETAINED);
    
    if (published) {
        Serial.printf("[MQTT] ✓ Publish successful\n");
    } else {
        Serial.println("[MQTT] ✗ Publish failed!");
    }
}

// Public method to publish battery data
void MQTTClient::publishBatteryData(const BatteryMonitor* monitor) {
    unsigned long now = millis();
    
    if (!monitor || !monitor->config) {
        return;
    }
    
    // Only publish when monitoring (device connected and receiving data)
    if (monitor->state != STATE_MONITORING) {
        return;
    }
    
    // Wait for valid data (voltage > 0 means we've received at least one notification)
    if (monitor->voltage <= 0.0f) {
        return;
    }
    
    // Publish discovery once on first publish
    static bool discoveryPublished[MAX_DEVICES] = {false};
    if (!discoveryPublished[monitor->configIndex]) {
        Serial.printf("[MQTT] Publishing Home Assistant discovery for %s\n", monitor->config->name);
        publishHomeAssistantDiscovery(monitor->config);
        discoveryPublished[monitor->configIndex] = true;
    }
    
    // Publish state
    publishState(monitor);
}

#endif // MQTT_ENABLED
