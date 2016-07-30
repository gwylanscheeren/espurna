/*

ESPurna
Copyright (C) 2016 by Xose Pérez <xose dot perez at gmail dot com>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#include <stdio.h>
#include <stdlib.h>

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <EEPROM.h>
#include "FS.h"

#include "Config.h"
#include "NoFUSSClient.h"
#include <DebounceEvent.h>
#include <EmonLiteESP.h>

#include <PubSubClient.h>
#include <RemoteReceiver.h>
#include <ArduinoJson.h>
#include "DHT.h"

// -----------------------------------------------------------------------------
// Configuració
// -----------------------------------------------------------------------------

#define DEBUG

//#define ESPURNA
//#define SONOFF
#define SLAMPHER
//#define S20

#define ENABLE_RF               0
#define ENABLE_OTA              1
#define ENABLE_NOFUSS           1
#define ENABLE_MQTT             1
#define ENABLE_WEBSERVER        1
#define ENABLE_POWERMONITOR     1
#define ENABLE_DHT              0

#define APP_NAME                "Espurna"
#define APP_VERSION             "0.9.4"
#define APP_AUTHOR              "xose.perez@gmail.com"
#define APP_WEBSITE             "http://tinkerman.cat"

#ifdef ESPURNA
    #define MANUFACTURER        "TINKERMAN"
    #define MODEL               "ESPURNA"
#endif

#ifdef SONOFF
    #define MANUFACTURER        "ITEAD"
    #define MODEL               "SONOFF"
#endif

#ifdef SLAMPHER
    #define MANUFACTURER        "ITEAD"
    #define MODEL               "SLAMPHER"
#endif

#ifdef S20
    #define MANUFACTURER        "ITEAD"
    #define MODEL               "S20"
#endif

#define BUTTON_PIN              0
#define RELAY_PIN               12
#define LED_PIN                 13

#define ADMIN_PASS              "fibonacci"

#define BUFFER_SIZE             1024
#define STATUS_UPDATE_INTERVAL  10000

#define RF_PIN                  14

#define DHT_PIN                 14
#define DHT_UPDATE_INTERVAL     300000
#define DHT_TYPE                DHT22
#define DHT_TIMING              11

#define MQTT_RECONNECT_DELAY    10000
#define MQTT_RETAIN             true

#define WIFI_CONNECT_TIMEOUT    5000
#define WIFI_RECONNECT_DELAY    2000

#define WIFI_STATUS_CONNECTING  0
#define WIFI_STATUS_CONNECTED   1
#define WIFI_STATUS_AP          2

#define CURRENT_PIN             0
#define ADC_BITS                10
#define REFERENCE_VOLTAGE       1.0
#define CURRENT_PRECISION       1
#define CURRENT_OFFSET          0.3
#define SAMPLES_X_MEASUREMENT   1000
#define MEASUREMENT_INTERVAL    10000
#define MEASUREMENTS_X_MESSAGE  6

// -----------------------------------------------------------------------------
// Globals
// -----------------------------------------------------------------------------

byte status = WIFI_STATUS_CONNECTING;

DebounceEvent button1 = false;

#if ENABLE_WEBSERVER
    ESP8266WebServer server(80);
#endif

#if ENABLE_MQTT
    void mqttSend(char * topic, char * message);
    WiFiClient client;
    PubSubClient mqtt(client);
    char mqttStatusTopic[40];
    char mqttIPTopic[40];
    #if ENABLE_DHT
        char mqttTemperatureTopic[40];
        char mqttHumidityTopic[40];
    #endif
    #if ENABLE_POWERMONITOR
        char mqttPowerTopic[40];
    #endif
    bool isMQTTMessage = false;
#endif

#if ENABLE_RF
    unsigned long rfCode = 0;
    unsigned long rfCodeON = 0;
    unsigned long rfCodeOFF = 0;
#endif

#if ENABLE_DHT
    DHT dht(DHT_PIN, DHT_TYPE, DHT_TIMING);
    double temperature;
    double humidity;
#endif

#if ENABLE_POWERMONITOR
    EmonLiteESP power;
    double current;
#endif

// -----------------------------------------------------------------------------
// Utils
// -----------------------------------------------------------------------------

char * getCompileTime(char * buffer) {

    int day, month, year, hour, minute, second;

    // parse date
    String tmp = String(__DATE__);
    day = tmp.substring(4,6).toInt();
    year = tmp.substring(7).toInt();
    tmp = tmp.substring(0,3);
    if (tmp.equals("Jan")) month = 1;
    if (tmp.equals("Feb")) month = 2;
    if (tmp.equals("Mar")) month = 3;
    if (tmp.equals("Apr")) month = 4;
    if (tmp.equals("May")) month = 5;
    if (tmp.equals("Jun")) month = 6;
    if (tmp.equals("Jul")) month = 7;
    if (tmp.equals("Aug")) month = 8;
    if (tmp.equals("Sep")) month = 9;
    if (tmp.equals("Oct")) month = 10;
    if (tmp.equals("Nov")) month = 11;
    if (tmp.equals("Dec")) month = 12;

    // parse time
    tmp = String(__TIME__);
    hour = tmp.substring(0,2).toInt();
    minute = tmp.substring(3,5).toInt();
    second = tmp.substring(6,8).toInt();

    sprintf(buffer, "%d%02d%02d%02d%02d%02d", year, month, day, hour, minute, second);
    buffer[14] = 0;
    return buffer;

}

String getIdentifier() {
    char identifier[20];
    sprintf(identifier, "%s_%06X", MODEL, ESP.getChipId());
    return String(identifier);
}

void blink(unsigned long delayOff, unsigned long delayOn) {
    static unsigned long next = millis();
    static bool status = HIGH;
    if (next < millis()) {
        status = !status;
        digitalWrite(LED_PIN, status);
        next += ((status) ? delayOff : delayOn);
    }
}

void showStatus() {
    if (WiFi.status() == WL_CONNECTED) {
        blink(5000, 500);
    } else {
        blink(500, 500);
    }
}

// -----------------------------------------------------------------------------
// Relay
// -----------------------------------------------------------------------------

void switchRelayOn() {

    if (!digitalRead(RELAY_PIN)) {

        #ifdef DEBUG
            Serial.println(F("[RELAY] ON"));
        #endif
        digitalWrite(RELAY_PIN, HIGH);
        EEPROM.write(0, 1);
        EEPROM.commit();

        #if ENABLE_MQTT
            if (!isMQTTMessage) mqttSend(mqttStatusTopic, (char *) "1");
        #endif

    }

}

void switchRelayOff() {

    if (digitalRead(RELAY_PIN)) {

        #ifdef DEBUG
            Serial.println(F("[RELAY] OFF"));
        #endif
        digitalWrite(RELAY_PIN, LOW);
        EEPROM.write(0, 0);
        EEPROM.commit();

        #if ENABLE_MQTT
            if (!isMQTTMessage) mqttSend(mqttStatusTopic, (char *) "0");
        #endif

    }

}

void toggleRelay() {
    if (digitalRead(RELAY_PIN)) {
        switchRelayOff();
    } else {
        switchRelayOn();
    }
}

// -----------------------------------------------------------------------------
// NoFUSS
// -----------------------------------------------------------------------------

#if ENABLE_NOFUSS

    void nofussSetup() {

        NoFUSSClient.setServer(config.nofussServer);
        NoFUSSClient.setDevice(MODEL);
        NoFUSSClient.setVersion(APP_VERSION);

        NoFUSSClient.onMessage([](nofuss_t code) {

            if (code == NOFUSS_START) {
                Serial.println(F("[NoFUSS] Start"));
            }

            if (code == NOFUSS_UPTODATE) {
                Serial.println(F("[NoFUSS] Already in the last version"));
            }

            if (code == NOFUSS_PARSE_ERROR) {
                Serial.println(F("[NoFUSS] Error parsing server response"));
            }

            if (code == NOFUSS_UPDATING) {
                Serial.println(F("[NoFUSS] Updating"));
                Serial.print(  F("         New version: "));
                Serial.println(NoFUSSClient.getNewVersion());
                Serial.print(  F("         Firmware: "));
                Serial.println(NoFUSSClient.getNewFirmware());
                Serial.print(  F("         File System: "));
                Serial.println(NoFUSSClient.getNewFileSystem());
            }

            if (code == NOFUSS_FILESYSTEM_UPDATE_ERROR) {
                Serial.print(F("[NoFUSS] File System Update Error: "));
                Serial.println(NoFUSSClient.getErrorString());
            }

            if (code == NOFUSS_FILESYSTEM_UPDATED) {
                Serial.println(F("[NoFUSS] File System Updated"));
            }

            if (code == NOFUSS_FIRMWARE_UPDATE_ERROR) {
                Serial.print(F("[NoFUSS] Firmware Update Error: "));
                Serial.println(NoFUSSClient.getErrorString());
            }

            if (code == NOFUSS_FIRMWARE_UPDATED) {
                Serial.println(F("[NoFUSS] Firmware Updated"));
            }

            if (code == NOFUSS_RESET) {
                Serial.println(F("[NoFUSS] Resetting board"));
            }

            if (code == NOFUSS_END) {
                Serial.println(F("[NoFUSS] End"));
            }

        });

    }

    void nofussLoop() {

        static unsigned long last_check = 0;
        if (WiFi.status() != WL_CONNECTED) return;
        if ((last_check > 0) && ((millis() - last_check) < config.nofussInterval.toInt())) return;
        last_check = millis();
        NoFUSSClient.handle();

    }

#endif


// -----------------------------------------------------------------------------
// OTA
// -----------------------------------------------------------------------------

#if ENABLE_OTA

    void OTASetup() {

        ArduinoOTA.setPort(8266);
        ArduinoOTA.setHostname(config.hostname.c_str());
        ArduinoOTA.setPassword((const char *) ADMIN_PASS);

        ArduinoOTA.onStart([]() {
            #if ENABLE_RF
                RemoteReceiver::disable();
            #endif
            #ifdef DEBUG
                Serial.println(F("[OTA] Start"));
            #endif
        });

        ArduinoOTA.onEnd([]() {
            #ifdef DEBUG
                Serial.println(F("[OTA] End"));
            #endif
            #if ENABLE_RF
                RemoteReceiver::enable();
            #endif
        });

        ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
            #ifdef DEBUG
                Serial.printf("[OTA] Progress: %u%%\r", (progress / (total / 100)));
            #endif
        });

        ArduinoOTA.onError([](ota_error_t error) {
            #ifdef DEBUG
                Serial.printf("[OTA] Error[%u]: ", error);
                if (error == OTA_AUTH_ERROR) Serial.println(F("[OTA] Auth Failed"));
                else if (error == OTA_BEGIN_ERROR) Serial.println(F("[OTA] Begin Failed"));
                else if (error == OTA_CONNECT_ERROR) Serial.println(F("[OTA] Connect Failed"));
                else if (error == OTA_RECEIVE_ERROR) Serial.println(F("[OTA] Receive Failed"));
                else if (error == OTA_END_ERROR) Serial.println(F("[OTA] End Failed"));
            #endif
        });

        ArduinoOTA.begin();

    }

    void OTALoop() {
        ArduinoOTA.handle();
    }

#endif

// -----------------------------------------------------------------------------
// Wifi
// -----------------------------------------------------------------------------

void wifiSetupAP() {

    // Set WIFI module AP mode
    WiFi.mode(WIFI_AP);
    #ifdef DEBUG
        WiFi.printDiag(Serial);
    #endif

    // SoftAP mode
    WiFi.softAP(config.hostname.c_str(), ADMIN_PASS);
    status = WIFI_STATUS_AP;
    delay(100);
    #ifdef DEBUG
        Serial.print(F("[WIFI] ACCESS POINT Mode, SSID: "));
        Serial.print(config.hostname);
        Serial.print(F(", Password: \""));
        Serial.print(ADMIN_PASS);
        Serial.print(F("\", IP address: "));
        Serial.println(WiFi.softAPIP());
    #endif

}

void wifiSetupSTA() {

    byte network = 0;

    // Set WIFI module to STA mode
    WiFi.mode(WIFI_STA);
    #ifdef DEBUG
        //WiFi.printDiag(Serial);
    #endif

    #if ENABLE_MQTT
        if (mqtt.connected()) mqtt.disconnect();
    #endif

    #if ENABLE_RF
        RemoteReceiver::disable();
    #endif

    while (network < 3) {

        if (config.ssid[network].length() > 0) {

            WiFi.begin(
                (const char *) config.ssid[network].c_str(),
                (const char *) config.pass[network].c_str()
            );

            #ifdef DEBUG
                Serial.print(F("[WIFI] Connecting to "));
                Serial.println(config.ssid[network]);
            #endif

            // Wait
            unsigned long timeout = millis() + WIFI_CONNECT_TIMEOUT;
            while (timeout > millis()) {
                showStatus();
                if (WiFi.status() == WL_CONNECTED) break;
                delay(100);
            }

        }

        if (WiFi.status() == WL_CONNECTED) break;
        network++;

    }

    #if ENABLE_RF
        RemoteReceiver::enable();
    #endif

    if (WiFi.status() == WL_CONNECTED) {

        WiFi.setAutoConnect(true);
        status = WIFI_STATUS_CONNECTED;

        #ifdef DEBUG
            Serial.print(F("[WIFI] STATION Mode, SSID: "));
            Serial.print(WiFi.SSID());
            Serial.print(F(", IP address: "));
            Serial.println(WiFi.localIP());
        #endif

        #if ENABLE_NOFUSS
            NoFUSSClient.handle();
        #endif

    } else {

        #ifdef DEBUG
            Serial.println(F("[WIFI] Not connected"));
        #endif

    }

}

void wifiSetup(bool force) {
    if (force || (WiFi.status() != WL_CONNECTED)) {
        wifiSetupSTA();
        if (WiFi.status() != WL_CONNECTED) wifiSetupAP();
    }
}

void wifiLoop() {

    static unsigned long last_check = 0;

    // Check disconnection
    if ((status == WIFI_STATUS_CONNECTED) && (WiFi.status() != WL_CONNECTED)) {
        status = WIFI_STATUS_CONNECTING;
    }

    // If not connected (either AP or STA) try to reconnect every WIFI_RECONNECT_DELAY
    if (status == WIFI_STATUS_CONNECTING) {
        if ((millis() - last_check) > WIFI_RECONNECT_DELAY) {
            wifiSetup(false);
            last_check = millis();
        }
    }

}

// -----------------------------------------------------------------------------
// RF
// -----------------------------------------------------------------------------

#if ENABLE_RF

    void rfLoop() {
        if (rfCode == 0) return;
        #ifdef DEBUG
            Serial.print(F("[RF] Received code: "));
            Serial.println(rfCode);
        #endif
        if (rfCode == rfCodeON) switchRelayOn();
        if (rfCode == rfCodeOFF) switchRelayOff();
        rfCode = 0;
    }

    void rfBuildCodes() {

        unsigned long code = 0;

        // channel
        unsigned int channel = config.rfChannel.toInt();
        for (byte i = 0; i < 5; i++) {
            code *= 3;
            if (channel & 1) code += 1;
            channel >>= 1;
        }

        // device
        unsigned int device = config.rfDevice.toInt();
        for (byte i = 0; i < 5; i++) {
            code *= 3;
            if (device != i) code += 2;
        }

        // status
        code *= 9;
        rfCodeOFF = code + 2;
        rfCodeON = code + 6;

        #ifdef DEBUG
            Serial.print(F("[RF] Code ON: "));
            Serial.println(rfCodeON);
            Serial.print(F("[RF] Code OFF: "));
            Serial.println(rfCodeOFF);
        #endif

    }

    void rfCallback(unsigned long code, unsigned int period) {
        rfCode = code;
    }

    void rfSetup() {
        pinMode(RF_PIN, INPUT_PULLUP);
        rfBuildCodes();
        RemoteReceiver::init(RF_PIN, 3, rfCallback);
        RemoteReceiver::enable();
    }

#endif

// -----------------------------------------------------------------------------
// WebServer
// -----------------------------------------------------------------------------

#if ENABLE_WEBSERVER

    String getContentType(String filename) {
        if (server.hasArg("download")) return "application/octet-stream";
        else if (filename.endsWith(".htm")) return "text/html";
        else if (filename.endsWith(".html")) return "text/html";
        else if (filename.endsWith(".css")) return "text/css";
        else if (filename.endsWith(".js")) return "application/javascript";
        else if (filename.endsWith(".png")) return "image/png";
        else if (filename.endsWith(".gif")) return "image/gif";
        else if (filename.endsWith(".jpg")) return "image/jpeg";
        else if (filename.endsWith(".ico")) return "image/x-icon";
        else if (filename.endsWith(".xml")) return "text/xml";
        else if (filename.endsWith(".pdf")) return "application/x-pdf";
        else if (filename.endsWith(".zip")) return "application/x-zip";
        else if (filename.endsWith(".gz")) return "application/x-gzip";
        return "text/plain";
    }

    void handleRelayOn() {
        #ifdef DEBUG
            Serial.println(F("[WEBSERVER] Request: /relay/on"));
        #endif
        switchRelayOn();
        server.send(200, "text/plain", "ON");
    }

    void handleRelayOff() {
        #ifdef DEBUG
            Serial.println(F("[WEBSERVER] Request: /relay/off"));
        #endif
        switchRelayOff();
        server.send(200, "text/plain", "OFF");
    }

    bool handleFileRead(String path) {

        #ifdef DEBUG
            Serial.print(F("[WEBSERVER] Request: "));
            Serial.println(path);
        #endif

        if (path.endsWith("/")) path += "index.html";
        String contentType = getContentType(path);
        String pathWithGz = path + ".gz";

        if (SPIFFS.exists(pathWithGz)) path = pathWithGz;
        if (SPIFFS.exists(path)) {
            File file = SPIFFS.open(path, "r");
            size_t sent = server.streamFile(file, contentType);
            size_t contentLength = file.size();
            file.close();
            return true;
        }

        return false;

    }

    void handleInit() {

        #ifdef DEBUG
            Serial.println("[WEBSERVER] Request: /init");
        #endif

        char buffer[64];
        char built[16];
        getCompileTime(built);
        sprintf(buffer, "%s %s built %s", APP_NAME, APP_VERSION, built);

        StaticJsonBuffer<1024> jsonBuffer;
        JsonObject& root = jsonBuffer.createObject();

        root["appname"] = String(buffer);
        root["manufacturer"] = String(MANUFACTURER);
        root["model"] = String(MODEL);
        root["hostname"] = config.hostname;
        root["network"] = (WiFi.status() == WL_CONNECTED) ? WiFi.SSID() : "ACCESS POINT";
        root["ip"] = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
        root["updateInterval"] = STATUS_UPDATE_INTERVAL;

        root["ssid0"] = config.ssid[0];
        root["pass0"] = config.pass[0];
        root["ssid1"] = config.ssid[1];
        root["pass1"] = config.pass[1];
        root["ssid2"] = config.ssid[2];
        root["pass2"] = config.pass[2];

        #if ENABLE_MQTT
            root["mqttServer"] = config.mqttServer;
            root["mqttPort"] = config.mqttPort;
            root["mqttUser"] = config.mqttUser;
            root["mqttPassword"] = config.mqttPassword;
            root["mqttTopic"] = config.mqttTopic;
        #endif

        #if ENABLE_RF
            root["rfChannel"] = config.rfChannel;
            root["rfDevice"] = config.rfDevice;
        #endif

        #if ENABLE_POWERMONITOR
            root["pwMainsVoltage"] = config.pwMainsVoltage;
            root["pwCurrentRatio"] = config.pwCurrentRatio;
        #endif

        String output;
        root.printTo(output);
        server.send(200, "text/json", output);

    }

    void handleStatus() {

        #ifdef DEBUG
            //Serial.println("[WEBSERVER] Request: /status");
        #endif

        StaticJsonBuffer<256> jsonBuffer;
        JsonObject& root = jsonBuffer.createObject();
        root["relay"] = digitalRead(RELAY_PIN) ? 1: 0;
        #if ENABLE_MQTT
            root["mqtt"] = mqtt.connected() ? 1: 0;
        #endif
        #if ENABLE_POWERMONITOR
            root["power"] = current * config.pwMainsVoltage.toFloat();
        #endif
        #if ENABLE_DHT
            root["temperature"] = temperature;
            root["humidity"] = humidity;
        #endif

        String output;
        root.printTo(output);
        server.send(200, "text/json", output);

    }

    void handleSave() {

        #ifdef DEBUG
            Serial.println(F("[WEBSERVER] Request: /save"));
        #endif

        if (server.hasArg("status")) {
            if (server.arg("status") == "1") {
                switchRelayOn();
            } else {
                switchRelayOff();
            }
        }

        if (server.hasArg("ssid0")) config.ssid[0] = server.arg("ssid0");
        if (server.hasArg("pass0")) config.pass[0] = server.arg("pass0");
        if (server.hasArg("ssid1")) config.ssid[1] = server.arg("ssid1");
        if (server.hasArg("pass1")) config.pass[1] = server.arg("pass1");
        if (server.hasArg("ssid2")) config.ssid[2] = server.arg("ssid2");
        if (server.hasArg("pass2")) config.pass[2] = server.arg("pass2");

        #if ENABLE_MQTT
            if (server.hasArg("mqttServer")) config.mqttServer = server.arg("mqttServer");
            if (server.hasArg("mqttPort")) config.mqttPort = server.arg("mqttPort");
            if (server.hasArg("mqttUser")) config.mqttUser = server.arg("mqttUser");
            if (server.hasArg("mqttPassword")) config.mqttPassword = server.arg("mqttPassword");
            if (server.hasArg("mqttTopic")) config.mqttTopic = server.arg("mqttTopic");
        #endif
        #if ENABLE_RF
            if (server.hasArg("rfChannel")) config.rfChannel = server.arg("rfChannel");
            if (server.hasArg("rfDevice")) config.rfDevice = server.arg("rfDevice");
        #endif
        #if ENABLE_POWERMONITOR
            if (server.hasArg("pwMainsVoltage")) config.pwMainsVoltage = server.arg("pwMainsVoltage");
            if (server.hasArg("pwCurrentRatio")) config.pwCurrentRatio = server.arg("pwCurrentRatio");
        #endif

        server.send(202, "text/json", "{}");

        config.save();
        #if ENABLE_RF
            rfBuildCodes();
        #endif
        #if ENABLE_POWERMONITOR
            power.setCurrentRatio(config.pwCurrentRatio.toFloat());
        #endif
        wifiSetup(true);

    }

    void webServerSetup() {

        // Relay control
        server.on("/relay/on", HTTP_GET, handleRelayOn);
        server.on("/relay/off", HTTP_GET, handleRelayOff);

        // Configuration page
        server.on("/init", HTTP_GET, handleInit);
        server.on("/status", HTTP_GET, handleStatus);
        server.on("/save", HTTP_POST, handleSave);

        // Anything else
        server.onNotFound([]() {

            // Hidden files
            #ifndef DEBUG
                if (server.uri().startsWith("/.")) {
                    server.send(403, "text/plain", "Forbidden");
                    return;
                }
            #endif

            // Existing files in SPIFFS
            if (!handleFileRead(server.uri())) {
                server.send(404, "text/plain", "NotFound");
                return;
            }

        });

        // Run server
        server.begin();

    }

    void webServerLoop() {
        server.handleClient();
    }

#endif

// -----------------------------------------------------------------------------
// MQTT
// -----------------------------------------------------------------------------

#if ENABLE_MQTT

    void buildTopics() {

        String tmp;

        // Replace identifier
        String base = config.mqttTopic;
        base.replace("{identifier}", config.hostname);

        // Get publish status topic
        base.toCharArray(mqttStatusTopic, base.length()+1);
        mqttStatusTopic[base.length()+1] = 0;

        // Get publish ip topic
        tmp = base + "/ip";
        tmp.toCharArray(mqttIPTopic, tmp.length()+1);
        mqttIPTopic[tmp.length()+1] = 0;

        // Get publish current topic
        #if ENABLE_POWERMONITOR
            tmp = base + "/power";
            tmp.toCharArray(mqttPowerTopic, tmp.length()+1);
            mqttPowerTopic[tmp.length()+1] = 0;
        #endif

        // Get publish current topic
        #if ENABLE_DHT
            tmp = base + "/temperature";
            tmp.toCharArray(mqttTemperatureTopic, tmp.length()+1);
            mqttTemperatureTopic[tmp.length()+1] = 0;
            tmp = base + "/humidity";
            tmp.toCharArray(mqttHumidityTopic, tmp.length()+1);
            mqttHumidityTopic[tmp.length()+1] = 0;
        #endif

    }

    void mqttSend(char * topic, char * message) {

        if (!mqtt.connected()) return;

        #ifdef DEBUG
            Serial.print(F("[MQTT] Sending "));
            Serial.print(topic);
            Serial.print(F(" "));
            Serial.println(message);
        #endif

        mqtt.publish(topic, message, MQTT_RETAIN);

    }

    void mqttCallback(char* topic, byte* payload, unsigned int length) {

        #ifdef DEBUG
            Serial.print(F("[MQTT] Received "));
            Serial.print(topic);
            Serial.print(F(" "));
            for (int i = 0; i < length; i++) {
                Serial.print((char)payload[i]);
            }
            Serial.println();
        #endif

        // Action to perform
        if ((char)payload[0] == '0') {
            isMQTTMessage = true;
            switchRelayOff();
        }
        if ((char)payload[0] == '1') {
            isMQTTMessage = true;
            switchRelayOn();
        }
        if ((char)payload[0] == '2') {
            toggleRelay();
        }
        isMQTTMessage = false;


    }

    void mqttConnect() {

        if (!mqtt.connected() && (config.mqttServer.length()>0)) {

            mqtt.setServer((const char *) config.mqttServer.c_str(), config.mqttPort.toInt());

            #ifdef DEBUG
                Serial.print(F("[MQTT] Connecting to broker at "));
                Serial.print(config.mqttServer);
            #endif

            if (config.mqttUser.length() > 0) {
                #ifdef DEBUG
                    Serial.print(F(" as user "));
                    Serial.print(config.mqttUser);
                    Serial.print(F(": "));
                #endif
                mqtt.connect(
                    config.hostname.c_str(),
                    (const char *) config.mqttUser.c_str(),
                    (const char *) config.mqttPassword.c_str()
                );
            } else {
                #ifdef DEBUG
                    Serial.print(F(" anonymously: "));
                #endif
                mqtt.connect(config.hostname.c_str());
            }

            if (mqtt.connected()) {

                #ifdef DEBUG
                    Serial.println(F("connected!"));
                #endif

                buildTopics();

                // Say hello and report our IP
                mqttSend(mqttIPTopic, (char *) WiFi.localIP().toString().c_str());

                // Publish current relay status
                mqttSend(mqttStatusTopic, (char *) (digitalRead(RELAY_PIN) ? "1" : "0"));

                // Subscribe to topic
                #ifdef DEBUG
                    Serial.print(F("[MQTT] Subscribing to "));
                    Serial.println(mqttStatusTopic);
                #endif
                mqtt.subscribe(mqttStatusTopic);


            } else {

                #ifdef DEBUG
                    Serial.print(F("failed, rc="));
                    Serial.println(mqtt.state());
                #endif

            }
        }

    }

    void mqttSetup() {
        mqtt.setCallback(mqttCallback);
    }

    void mqttLoop() {
        static unsigned long timeout = millis();
        if (WiFi.status() == WL_CONNECTED) {
            if (!mqtt.connected()) {
                if (timeout < millis()) {
                    mqttConnect();
                    timeout = millis() + MQTT_RECONNECT_DELAY;
                }
            }
            if (mqtt.connected()) mqtt.loop();
        }
    }

#endif

// -----------------------------------------------------------------------------
// DHT
// -----------------------------------------------------------------------------

#if ENABLE_MQTT
#if ENABLE_DHT

    void dhtSetup() {
        dht.begin();
    }

    void dhtLoop() {

        static unsigned long last_check = 0;
        if (!mqtt.connected()) return;
        if ((last_check > 0) && ((millis() - last_check) < DHT_UPDATE_INTERVAL)) return;
        last_check = millis();

        char buffer[10];

        temperature = dht.readTemperature();
        if (isnan(temperature)) {
            #ifdef DEBUG
                Serial.println(F("[DHT] Error reading temperature"));
            #endif
        } else {
            dtostrf(temperature, 4, 1, buffer);
            mqttSend(mqttTemperatureTopic, buffer);
            #ifdef DEBUG
                Serial.print(F("[DHT] Temperature: "));
                Serial.println(temperature);
            #endif
        }

        humidity = dht.readHumidity();
        if (isnan(humidity)) {
            #ifdef DEBUG
                Serial.println(F("[DHT] Error reading humidity"));
            #endif
        } else {
            dtostrf(humidity, 4, 1, buffer);
            mqttSend(mqttHumidityTopic, buffer);
            #ifdef DEBUG
                Serial.print(F("[DHT] Humidity: "));
                Serial.println(humidity);
            #endif
        }

    }

#endif
#endif

// -----------------------------------------------------------------------------
// Energy Monitor
// -----------------------------------------------------------------------------

#if ENABLE_POWERMONITOR
#if ENABLE_MQTT

    unsigned int currentCallback() {
        return analogRead(CURRENT_PIN);
    }

    void powerMonitorSetup() {
        power.initCurrent(currentCallback, ADC_BITS, REFERENCE_VOLTAGE, config.pwCurrentRatio.toFloat());
        power.setPrecision(CURRENT_PRECISION);
    }

    void powerMonitorLoop() {

        static unsigned long next_measurement = millis();
        static byte measurements = 0;
        static double max = 0;
        static double min = 0;
        static double sum = 0;

        if (!mqtt.connected()) return;

        if (millis() > next_measurement) {

            // Safety check: do not read current if relay is OFF
            if (!digitalRead(RELAY_PIN)) {
                current = 0;
            } else {
                current = power.getCurrent(SAMPLES_X_MEASUREMENT);
                current -= CURRENT_OFFSET;
            }

            if (measurements == 0) {
                max = min = current;
            } else {
                if (current > max) max = current;
                if (current < min) min = current;
            }
            sum += current;
            ++measurements;

            #ifdef DEBUG
                Serial.print(F("[ENERGY] Power now: "));
                Serial.print(int(current * config.pwMainsVoltage.toFloat()));
                Serial.println(F("W"));
            #endif

            if (measurements == MEASUREMENTS_X_MESSAGE) {
                char buffer[8];
                double power = (sum - max - min) * config.pwMainsVoltage.toFloat() / (measurements - 2);
                sprintf(buffer, "%d", int(power));
                mqttSend(mqttPowerTopic, buffer);
                sum = 0;
                measurements = 0;
            }

            next_measurement += MEASUREMENT_INTERVAL;

        }

    }

#endif
#endif

// -----------------------------------------------------------------------------
// Hardware (buttons, LEDs,...)
// -----------------------------------------------------------------------------

void hardwareSetup() {
    Serial.begin(115200);
    pinMode(RELAY_PIN, OUTPUT);
    pinMode(LED_PIN, OUTPUT);
    button1 = DebounceEvent(BUTTON_PIN);
    SPIFFS.begin();
    EEPROM.begin(4096);
    EEPROM.read(0) == 1 ? switchRelayOn() : switchRelayOff();
}

void hardwareLoop() {
    if (button1.loop()) {
        if (button1.getEvent() == EVENT_SINGLE_CLICK) toggleRelay();
        if (button1.getEvent() == EVENT_LONG_CLICK) wifiSetupAP();
        if (button1.getEvent() == EVENT_DOUBLE_CLICK) ESP.reset();
    }
    showStatus();
}

// -----------------------------------------------------------------------------
// Booting
// -----------------------------------------------------------------------------

void welcome() {
    char buffer[BUFFER_SIZE];
    getCompileTime(buffer);
    Serial.println();
    Serial.println();
    Serial.print(APP_NAME);
    Serial.print(F(" "));
    Serial.print(APP_VERSION);
    Serial.print(F(" built "));
    Serial.println(buffer);
    Serial.println(APP_AUTHOR);
    Serial.println(APP_WEBSITE);
    Serial.println();
    Serial.print(F("Device: "));
    Serial.println(getIdentifier());
    Serial.print(F("Last reset reason: "));
    Serial.println(ESP.getResetReason());
    FSInfo fs_info;
    if (SPIFFS.info(fs_info)) {
        Serial.print("File system total size: ");
        Serial.println(fs_info.totalBytes);
        Serial.print("File system used size : ");
        Serial.println(fs_info.usedBytes);
    }

    Serial.println();
}

void setup() {

    hardwareSetup();
    delay(1000);
    welcome();
    config.hostname = getIdentifier();
    config.load();

    // We are handling first connection in the loop
    //wifiSetup(false);

    #if ENABLE_OTA
        OTASetup();
    #endif
    #if ENABLE_NOFUSS
        nofussSetup();
    #endif
    #if ENABLE_MQTT
        mqttSetup();
    #endif
    #if ENABLE_WEBSERVER
        webServerSetup();
    #endif
    #if ENABLE_RF
        rfSetup();
    #endif
    #if ENABLE_DHT
        dhtSetup();
    #endif
    #if ENABLE_POWERMONITOR
        powerMonitorSetup();
    #endif

}

void loop() {

    wifiLoop();
    hardwareLoop();

    #if ENABLE_OTA
        OTALoop();
    #endif
    #if ENABLE_NOFUSS
        nofussLoop();
    #endif
    #if ENABLE_MQTT
        mqttLoop();
    #endif
    #if ENABLE_WEBSERVER
        webServerLoop();
    #endif
    #if ENABLE_RF
        rfLoop();
    #endif
    #if ENABLE_DHT
        dhtLoop();
    #endif
    #if ENABLE_POWERMONITOR
        powerMonitorLoop();
    #endif

    delay(1);

}