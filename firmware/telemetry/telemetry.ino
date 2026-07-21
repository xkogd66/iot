// MXChip AZ3166 home telemetry — Milestone 3
// Read sensors, connect Wi-Fi, publish JSON telemetry over MQTT to Mosquitto.
// Topic: sensors/<site>/<room>  Payload: {"temp":..,"humidity":..,"pressure":..,"rssi":..}
//
// Device identity (Wi-Fi, MQTT broker, site/room) is no longer compiled in —
// it's provisioned over a SoftAP + web form (see config_portal.h) and
// persisted on the STSAFE-A100 secure element (see device_config.h). A board
// with no saved config, or one booted with USER_BUTTON_B held, enters that
// setup portal instead of the normal publish loop below.
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
#include "device_config.h"
#include "config_portal.h"

static DevI2C *i2c = NULL;
static HTS221Sensor *tempHumSensor = NULL;
static LPS22HBSensor *pressureSensor = NULL;

static MQTTNetwork *mqttNetwork = NULL;
static MQTT::Client<MQTTNetwork, Countdown, 256> *mqttClient = NULL;

static DeviceConfig config;
static char mqttTopic[64];
static char mqttClientId[80];
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
    Serial.printf("Connecting to Wi-Fi SSID '%s'...\r\n", config.wifiSsid);
    oledStatus("IoT Telemetry", "WiFi connecting", config.wifiSsid);
    if (WiFi.begin(config.wifiSsid, config.wifiPassword) != WL_CONNECTED)
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
    Serial.printf("Connecting to MQTT %s:%d ...\r\n", config.mqttHost, config.mqttPort);
    oledStatus("IoT Telemetry", "MQTT connecting", config.mqttHost);

    if (mqttClient != NULL) { delete mqttClient; mqttClient = NULL; }
    if (mqttNetwork != NULL) { delete mqttNetwork; mqttNetwork = NULL; }

    mqttNetwork = new MQTTNetwork();
    int rc = mqttNetwork->connect(config.mqttHost, config.mqttPort);
    if (rc != 0)
    {
        Serial.printf("TCP connect failed: %d\r\n", rc);
        oledStatus("IoT Telemetry", "MQTT TCP fail", NULL);
        return false;
    }

    mqttClient = new MQTT::Client<MQTTNetwork, Countdown, 256>(*mqttNetwork);
    MQTTPacket_connectData options = MQTTPacket_connectData_initializer;
    options.MQTTVersion = 4;  // 3.1.1
    options.clientID.cstring = mqttClientId;
    options.keepAliveInterval = 60;
    options.cleansession = 1;

    rc = mqttClient->connect(options);
    if (rc != 0)
    {
        Serial.printf("MQTT connect failed: %d\r\n", rc);
        oledStatus("IoT Telemetry", "MQTT CONN fail", NULL);
        return false;
    }
    Serial.printf("MQTT connected. Publishing to %s\r\n", mqttTopic);
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

    bool hasConfig = loadDeviceConfig(config);

    pinMode(USER_BUTTON_B, INPUT);
    bool buttonHeld = digitalRead(USER_BUTTON_B) == LOW;

    if (!hasConfig || buttonHeld)
    {
        runConfigPortal(config);  // never returns — saves + reboots, or times out + reboots
    }

    snprintf(mqttTopic, sizeof(mqttTopic), "sensors/%s/%s", config.site, config.room);
    buildClientId(config.site, config.room, mqttClientId, sizeof(mqttClientId));

    // Connecting happens in loop()'s ensureConnected() on the first iteration,
    // not here — that's what lets the Button B check there interrupt even the
    // very first connect attempt, not just later retries.
}

void loop()
{
    // Checked every iteration (not just at boot) so Button B can break out of
    // a stuck connect-retry loop too — e.g. a misspelled Wi-Fi password that
    // would otherwise retry forever with no other way back into setup mode.
    if (digitalRead(USER_BUTTON_B) == LOW)
    {
        runConfigPortal(config); // never returns
    }

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

    int rc = mqttClient->publish(mqttTopic, payload, len, MQTT::QOS0);

    char l1[24], l2[24];
    snprintf(l1, sizeof(l1), "T%.1f H%.0f", temperature, humidity);
    snprintf(l2, sizeof(l2), rc == 0 ? "sent P%.0f" : "PUB ERR %d",
             rc == 0 ? pressure : (float)rc);
    oledStatus("IoT Telemetry", l1, l2);

    if (rc == 0)
        Serial.printf("published: %s %s\r\n", mqttTopic, payload);
    else
        Serial.printf("publish failed: %d\r\n", rc);

    mqttClient->yield(PUBLISH_INTERVAL_MS);
}
