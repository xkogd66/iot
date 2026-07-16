// MXChip AZ3166 home telemetry — Milestone 2
// Read sensors, connect Wi-Fi, publish JSON telemetry over MQTT to Mosquitto.
// Topic: sensors/<SITE>/<ROOM>  Payload: {"temp":..,"humidity":..,"pressure":..,"rssi":..}
//
// Build with arduino-cli (GCC 5.4) — see firmware/README.md. The AZ3166's Wi-Fi
// stack requires the matching bootloader flashed to 0x08000000 (once per board);
// see flash-bootloader.sh. Without it the app crashes at boot in Wi-Fi init.

#include "OledDisplay.h"
#include "HTS221Sensor.h"
#include "LPS22HBSensor.h"
#include "AZ3166WiFi.h"
#include "MQTTNetwork.h"
#include "MQTTClient.h"
#include "secrets.h"

static DevI2C *i2c = NULL;
static HTS221Sensor *tempHumSensor = NULL;
static LPS22HBSensor *pressureSensor = NULL;

static MQTTNetwork *mqttNetwork = NULL;
static MQTT::Client<MQTTNetwork, Countdown, 256> *mqttClient = NULL;

static const char *MQTT_TOPIC = "sensors/" SITE "/" ROOM;
static const int PUBLISH_INTERVAL_MS = 10000;  // 10s

static void oledStatus(const char *l0, const char *l1, const char *l2)
{
    Screen.clean();
    if (l0) Screen.print(0, l0);
    if (l1) Screen.print(1, l1);
    if (l2) Screen.print(2, l2);
}

static bool connectWiFi()
{
    Serial.printf("Connecting to Wi-Fi SSID '%s'...\r\n", WIFI_SSID);
    oledStatus("IoT Telemetry", "WiFi connecting", WIFI_SSID);
    if (WiFi.begin(WIFI_SSID, WIFI_PASSWORD) != WL_CONNECTED)
    {
        Serial.println("Wi-Fi connect FAILED");
        oledStatus("IoT Telemetry", "WiFi FAILED", NULL);
        return false;
    }
    char buf[24];
    snprintf(buf, sizeof(buf), "RSSI %d dBm", WiFi.RSSI());
    Serial.printf("Wi-Fi connected. %s\r\n", buf);
    oledStatus("IoT Telemetry", "WiFi OK", buf);
    return true;
}

static bool connectMQTT()
{
    Serial.printf("Connecting to MQTT %s:%d ...\r\n", MQTT_BROKER_HOST, MQTT_BROKER_PORT);
    oledStatus("IoT Telemetry", "MQTT connecting", MQTT_BROKER_HOST);

    if (mqttClient != NULL) { delete mqttClient; mqttClient = NULL; }
    if (mqttNetwork != NULL) { delete mqttNetwork; mqttNetwork = NULL; }

    mqttNetwork = new MQTTNetwork();
    int rc = mqttNetwork->connect(MQTT_BROKER_HOST, MQTT_BROKER_PORT);
    if (rc != 0)
    {
        Serial.printf("TCP connect failed: %d\r\n", rc);
        oledStatus("IoT Telemetry", "MQTT TCP fail", NULL);
        return false;
    }

    mqttClient = new MQTT::Client<MQTTNetwork, Countdown, 256>(*mqttNetwork);
    MQTTPacket_connectData options = MQTTPacket_connectData_initializer;
    options.MQTTVersion = 4;  // 3.1.1
    options.clientID.cstring = (char *)MQTT_CLIENT_ID;
    options.keepAliveInterval = 60;
    options.cleansession = 1;

    rc = mqttClient->connect(options);
    if (rc != 0)
    {
        Serial.printf("MQTT connect failed: %d\r\n", rc);
        oledStatus("IoT Telemetry", "MQTT CONN fail", NULL);
        return false;
    }
    Serial.printf("MQTT connected. Publishing to %s\r\n", MQTT_TOPIC);
    return true;
}

static bool ensureConnected()
{
    if (WiFi.status() != WL_CONNECTED && !connectWiFi()) return false;
    if (mqttClient == NULL || !mqttClient->isConnected()) return connectMQTT();
    return true;
}

void setup()
{
    Serial.begin(115200);
    Screen.init();
    oledStatus("IoT Telemetry", "Init sensors...", NULL);

    i2c = new DevI2C(D14, D15);
    tempHumSensor = new HTS221Sensor(*i2c);
    tempHumSensor->init(NULL);
    tempHumSensor->enable();
    pressureSensor = new LPS22HBSensor(*i2c);
    pressureSensor->init(NULL);

    connectWiFi();
    connectMQTT();
}

void loop()
{
    if (!ensureConnected())
    {
        delay(3000);
        return;
    }

    float temperature = 0.0f, humidity = 0.0f, pressure = 0.0f;
    tempHumSensor->getTemperature(&temperature);
    tempHumSensor->getHumidity(&humidity);
    pressureSensor->getPressure(&pressure);
    int rssi = WiFi.RSSI();

    char payload[96];
    int len = snprintf(payload, sizeof(payload),
                       "{\"temp\":%.1f,\"humidity\":%.1f,\"pressure\":%.1f,\"rssi\":%d}",
                       temperature, humidity, pressure, rssi);

    int rc = mqttClient->publish(MQTT_TOPIC, payload, len, MQTT::QOS0);

    char l1[24], l2[24];
    snprintf(l1, sizeof(l1), "T%.1f H%.0f", temperature, humidity);
    snprintf(l2, sizeof(l2), rc == 0 ? "sent P%.0f" : "PUB ERR %d",
             rc == 0 ? pressure : (float)rc);
    oledStatus("IoT Telemetry", l1, l2);

    if (rc == 0)
        Serial.printf("published: %s %s\r\n", MQTT_TOPIC, payload);
    else
        Serial.printf("publish failed: %d\r\n", rc);

    mqttClient->yield(PUBLISH_INTERVAL_MS);
}
