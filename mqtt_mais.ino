#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <SoftwareSerial.h>

#define device_id 4  // Device ID number!!! //TODO Change Device Number

// WiFi & MQTT Setup
const char* ssid = "tp-link";
const char* password = "09270734452";
const char* mqtt_server = "157.245.204.46";

WiFiClient espClient;
PubSubClient client(espClient);

#define DHTTYPE DHT11
#define DHTPin 0            // DHT11 sensor pin
#define SoilMoisturePin A0  // Soil Moisture Sensor pin
#define DS18B20_PIN 4       // DS18B20 sensor pin (soil temperature)
#define BUZZER_PIN 15       // D8

// RS485 Soil pH Sensor
#define DE 12
#define RE 14
SoftwareSerial mod(13, 5);  // RO on GPIO13 (D7), DI on GPIO5 (D1)
const byte ph_request[] = { 0x01, 0x03, 0x00, 0x00, 0x00, 0x01, 0x84, 0x0A };
byte values[11];

DHT dht(DHTPin, DHTTYPE);
OneWire oneWire(DS18B20_PIN);
DallasTemperature DS18B20(&oneWire);

#define SENSOR_TOPIC "sensor/device4/data"  // Single topic for JSON data //TODO Change Device Number

unsigned long lastSensorSend = 0;
const unsigned long SENSOR_INTERVAL = 5000;  // 5 seconds

void setup() {
  Serial.begin(115200);

  dht.begin();
  DS18B20.begin();
  mod.begin(4800);
  pinMode(BUZZER_PIN, OUTPUT);
  noTone(BUZZER_PIN); 

  pinMode(DE, OUTPUT);
  pinMode(RE, OUTPUT);
  digitalWrite(DE, LOW);
  digitalWrite(RE, LOW);

  // Initialize WiFi using WiFiManager
  WiFiManager wifiManager;

  // Uncomment to reset saved credentials (for testing)
  wifiManager.resetSettings();

  if (!wifiManager.autoConnect("MAIS-DEVICE4")) { //TODO Change Device Number
    Serial.println("Failed to connect and hit timeout");
    ESP.restart();
    delay(1000);
  }


  // WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected!");

  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);
  reconnect();
}

void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  unsigned long now = millis();
  if (now - lastSensorSend > SENSOR_INTERVAL) {
    lastSensorSend = now;
    sendSensorData();  // Now only runs every 5 seconds
  } // 5 seconds
}

// 🌱 Soil Moisture Data (Returns Raw & Percentage)
void soilMoistureData(float data[2]) {
  int rawValue = analogRead(SoilMoisturePin);
  float moisture_percentage = 100.00 - ((rawValue / 1023.00) * 100.00);

  data[0] = rawValue;
  data[1] = moisture_percentage;
}

// 🌡️ Temperature & Humidity Data (Returns Temp & Humidity)
bool temperatureData(float data[2]) {
  float humidity = dht.readHumidity();
  float temperature = dht.readTemperature();

  if (isnan(humidity) || isnan(temperature)) {
    Serial.println("❌ Failed to read from DHT sensor!");
    return false;
  }

  data[0] = temperature;
  data[1] = humidity;
  return true;
}

// 🌎 Soil Temperature from DS18B20
float getSoilTemperature() {
  DS18B20.requestTemperatures();
  return DS18B20.getTempCByIndex(0);
}

// 📏 Soil pH Measurement (Averaged)
float getSoilPH() {
  const int NUM_READINGS = 5;
  float totalPH = 0;
  int validReadings = 0;

  for (int i = 0; i < NUM_READINGS; i++) {
    digitalWrite(DE, HIGH);
    digitalWrite(RE, HIGH);
    delay(10);

    if (mod.write(ph_request, sizeof(ph_request)) == 8) {
      digitalWrite(DE, LOW);
      digitalWrite(RE, LOW);
      delay(100);

      for (byte j = 0; j < 11; j++) {
        values[j] = mod.read();
      }

      // Serial.print("Raw Data: ");
      for (byte j = 0; j < 11; j++) {
        // Serial.print(values[j], HEX);
        // Serial.print(" ");
      }
      // Serial.println();

      int raw_ph = (values[3] << 8) | values[4];
      float soil_ph = raw_ph / 10.0;

      if (soil_ph >= 3.5 && soil_ph <= 9.0) {
        totalPH += soil_ph;
        validReadings++;
      }
    }

    delay(500);
  }

  return (validReadings > 0) ? (totalPH / validReadings) : -1;
}

// 📡 Send All Sensor Data as JSON
void sendSensorData() {
  float soilData[2];
  float tempData[2];
  float soilTemperature = getSoilTemperature();
  float soilPH = getSoilPH();

  soilMoistureData(soilData);
  bool validTemp = temperatureData(tempData);

  // Create JSON object
  StaticJsonDocument<256> doc;
  doc["device_id"] = device_id;
  if (validTemp) {
    doc["temperature"] = tempData[0];
    doc["humidity"] = tempData[1];
  }
  doc["soil_moisture_raw"] = soilData[0];
  doc["soil_moisture_percentage"] = soilData[1];
  doc["soil_temperature"] = soilTemperature;
  if (soilPH != -1) {
    doc["soil_ph"] = soilPH;
  } else {
    doc["soil_ph"] = "N/A";  // Invalid readings
  }

  // Serialize JSON to a string
  char jsonBuffer[256];
  serializeJson(doc, jsonBuffer);

  Serial.print("📡 Sending JSON: ");
  Serial.println(jsonBuffer);

  // Publish JSON to MQTT
  client.publish(SENSOR_TOPIC, jsonBuffer);
}

void callback(char* topic, byte* payload, unsigned int length) {
  String message;
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  Serial.print("MQTT Message Received: ");
  Serial.println(message);

  Serial.print("Message arrived on topic: ");
  Serial.println(topic);

  if (strcmp(topic, "mais/animal") == 0) {
    StaticJsonDocument<64> doc;
    DeserializationError error = deserializeJson(doc, payload, length);

    if (error) {
      Serial.println("❌ Failed to parse JSON in animal-detection!");
      return;
    }

    bool hasAnimal = doc["has_animal"];

    if (hasAnimal) {
      Serial.println("🚨 Animal detected! Buzzing...");
  tone(BUZZER_PIN, 1000); // Frequency of 1000 Hz
      delay(5000);  // Buzz for 5 seconds
  noTone(BUZZER_PIN);   // Stop the tone
    }
  }
}

// 🔄 Reconnect to MQTT
void reconnect() {
  while (!client.connected()) {
    Serial.print("Connecting to MQTT...");
    if (client.connect("Device4")) { //TODO Change Device Number
      Serial.println("✅ Connected!");
    
      client.subscribe("mais/animal");
      if (client.subscribe("mais/animal")) {
        Serial.println("✅ Subscribed to mais/animal");
      } else {
        Serial.println("❌ Failed to subscribe to mais/animal");
      }

    } else {
      Serial.print("❌ Failed, rc=");
      Serial.print(client.state());
      Serial.println(" Retrying...");
      delay(2000);
    }
  }
}
