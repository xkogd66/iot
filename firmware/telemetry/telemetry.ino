// MXChip AZ3166 home telemetry — Milestone 2
// Read sensors, connect Wi-Fi, publish JSON telemetry over MQTT to Mosquitto.
// Topic: sensors/<SITE>/<ROOM>  Payload: {"temp":..,"humidity":..,"pressure":..,"rssi":..}
//
// Build with arduino-cli (GCC 5.4) — see firmware/README.md. The AZ3166's Wi-Fi
// stack requires the matching bootloader flashed to 0x08000000 (once per board);
// see flash-bootloader.sh. Without it the app crashes at boot in Wi-Fi init.
//
// B + reset opens OUR config portal (Wi-Fi + broker + site/room/id), not
// Microsoft's — see __sys_setup().

#include "OledDisplay.h"
#include "HTS221Sensor.h"
#include "LPS22HBSensor.h"
#include "AZ3166WiFi.h"
#include "MQTTNetwork.h"
#include "MQTTClient.h"
#include "EEPROMInterface.h"
#include "SystemWiFi.h"
#include "TCPServer.h"
#include "TCPSocket.h"
#include "secrets.h"
#include "portal_css.h"
#include <string.h>
#include <stdlib.h>

// Runtime config so a board can be re-homed without a rebuild. Stored as
//   host=iot.ekskog.net;port=1883;site=pueblodry;room=livingroom;id=az3166-kitchen
// Any key omitted falls back to the matching secrets.h define. The OLED shows
// the resulting identity at boot, so what the board connects as is never a guess.
//
// ponytail: stored in the Azure IoT Hub EEPROM zone purely because it is a spare
// 512-byte zone. If a real Azure connection string is ever needed on the same
// board, this needs its own zone.
static char cfgHost[64];
static int  cfgPort;
static char cfgSite[32];
static char cfgRoom[32];
static char cfgId[48];
static char cfgTopic[80];
static bool cfgFromEEPROM = false;

static DevI2C *i2c = NULL;
static HTS221Sensor *tempHumSensor = NULL;
static LPS22HBSensor *pressureSensor = NULL;

static MQTTNetwork *mqttNetwork = NULL;
static MQTT::Client<MQTTNetwork, Countdown, 256> *mqttClient = NULL;

static const int PUBLISH_INTERVAL_MS = 10000;  // 10s

// Pull "key=value" out of a ';'-separated config string; dflt if absent.
static void cfgField(const char *src, const char *key, char *out, size_t outLen,
                     const char *dflt)
{
    size_t klen = strlen(key);
    for (const char *p = src; p && *p; )
    {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=')
        {
            p += klen + 1;
            const char *end = strchr(p, ';');
            size_t n = end ? (size_t)(end - p) : strlen(p);
            if (n >= outLen) n = outLen - 1;
            memcpy(out, p, n);
            out[n] = '\0';
            if (n > 0) return;
            break;              // present but empty -> use the default
        }
        p = strchr(p, ';');
        if (p) p++;
    }
    strncpy(out, dflt, outLen - 1);
    out[outLen - 1] = '\0';
}

static void loadConfig()
{
    char raw[AZ_IOT_HUB_MAX_LEN + 1];
    memset(raw, 0, sizeof(raw));

    EEPROMInterface eeprom;
    int n = eeprom.read((uint8_t *)raw, AZ_IOT_HUB_MAX_LEN, 0, AZ_IOT_HUB_ZONE_IDX);
    if (n < 0) n = 0;
    raw[n] = '\0';
    cfgFromEEPROM = (strchr(raw, '=') != NULL);

    cfgField(raw, "host", cfgHost, sizeof(cfgHost), MQTT_BROKER_HOST);
    cfgField(raw, "site", cfgSite, sizeof(cfgSite), SITE);
    cfgField(raw, "room", cfgRoom, sizeof(cfgRoom), ROOM);
    cfgField(raw, "id",   cfgId,   sizeof(cfgId),   MQTT_CLIENT_ID);

    char portBuf[8], dfltPort[8];
    snprintf(dfltPort, sizeof(dfltPort), "%d", MQTT_BROKER_PORT);
    cfgField(raw, "port", portBuf, sizeof(portBuf), dfltPort);
    cfgPort = atoi(portBuf);
    if (cfgPort <= 0) cfgPort = MQTT_BROKER_PORT;

    snprintf(cfgTopic, sizeof(cfgTopic), "sensors/%s/%s", cfgSite, cfgRoom);
}

static void oledStatus(const char *l0, const char *l1, const char *l2)
{
    Screen.clean();
    if (l0) Screen.print(0, l0);
    if (l1) Screen.print(1, l1);
    if (l2) Screen.print(2, l2);
}

// ---------------------------------------------------------------------------
// Config portal, served from __sys_setup() when B is held through reset.
//
// Two things make this work that did not before:
//   1. __sys_setup() runs BEFORE main()'s IsAPMode() check (_main_sys.cpp:153),
//      so handling B here means the core's Microsoft page is never reached.
//   2. The Arduino WiFiServer class opens on WiFiInterface() — the *station*
//      interface (AZ3166WiFiServer.cpp:44). A phone joined to our soft AP talks
//      to WiFiAPInterface(), so WiFiServer can never serve a portal. We use
//      TCPServer directly against the AP interface instead.
// ---------------------------------------------------------------------------

static String urlDecode(const String &s)
{
    String out;
    for (unsigned int i = 0; i < s.length(); i++)
    {
        char c = s[i];
        if (c == '+') out += ' ';
        else if (c == '%' && i + 2 < s.length())
        {
            char hex[3] = { s[i + 1], s[i + 2], 0 };
            out += (char)strtol(hex, NULL, 16);
            i += 2;
        }
        else out += c;
    }
    return out;
}

// Value for `key` in a query string, "" if absent. Only matches whole keys.
static String qsGet(const String &qs, const char *key)
{
    String k = String(key) + "=";
    int i = qs.indexOf(k);
    while (i > 0 && qs[i - 1] != '&') i = qs.indexOf(k, i + 1);
    if (i < 0) return String();
    int start = i + k.length();
    int end = qs.indexOf('&', start);
    return urlDecode(qs.substring(start, end < 0 ? (int)qs.length() : end));
}

// mini.css input row, matching the core page's markup.
static String field(const char *placeholder, const char *name, const char *value)
{
    return "<div class=\"input-group fluid\"><input type=\"text\" name=\"" + String(name) +
           "\" value=\"" + value + "\" placeholder=\"" + placeholder + "\"></div>";
}

// Body only — PORTAL_HEAD supplies doctype, meta and the mini.css <style>.
static String portalPage(const char *ssid)
{
    String h = "<body><header><h1 class=\"logo\">EkSkog IoT Settings</h1></header>"
               "<section class=\"container\"><div id=\"content\" class=\"row\">"
               "<div class=\"col-sm-10 col-sm-offset-1 col-md-4 col-md-offset-4\" style=\"text-align:center;\">"
               "<form action=\"/save\"><div><fieldset><legend>Wi-Fi Settings</legend>";
    h += field("SSID", "ssid", ssid);
    h += "<div class=\"input-group fluid\"><input type=\"password\" name=\"pwd\" "
         "placeholder=\"Password (blank = unchanged)\"></div></fieldset></div>";

    h += "<div><fieldset><legend>MQTT Settings</legend>";
    h += field("Broker host", "host", cfgHost);
    char portBuf[8];
    snprintf(portBuf, sizeof(portBuf), "%d", cfgPort);
    h += field("Port", "port", portBuf);
    h += field("Site", "site", cfgSite);
    h += field("Room", "room", cfgRoom);
    h += field("Client id", "id", cfgId);
    h += "</fieldset></div>";

    h += "<div class=\"input-group fluid\">"
         "<button type=\"submit\" class=\"primary\">Save &amp; reboot</button></div></form>"
         "<h5 style=\"color:#616161;\">Publishes to sensors/&lt;site&gt;/&lt;room&gt;. "
         "Client id must be unique per board.</h5>"
         "</div></div></section></body></html>";
    return h;
}

// PORTAL_HEAD is ~5KB of mini.css, so it is sent straight from flash rather than
// concatenated into the body String.
static void sendAndClose(TCPSocket &client, const String &body)
{
    size_t headLen = strlen(PORTAL_HEAD);
    String hdr = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: " +
                 String((unsigned)(headLen + body.length())) +
                 "\r\nConnection: close\r\n\r\n";
    client.send(hdr.c_str(), hdr.length());
    client.send(PORTAL_HEAD, headLen);
    client.send(body.c_str(), body.length());
    client.close();
}

static void runConfigPortal()
{
    loadConfig();

    char apSsid[24];
    snprintf(apSsid, sizeof(apSsid), "EkSkog-%s", cfgRoom);

    oledStatus("EkSkog IoT", "starting AP", NULL);
    if (!InitSystemWiFi())      { oledStatus("EkSkog IoT", "wifi init FAIL", NULL); return; }
    if (!SystemWiFiAPStart(apSsid, "")) { oledStatus("EkSkog IoT", "AP start FAIL", NULL); return; }

    char curSsid[WIFI_SSID_MAX_LEN + 1] = { 0 };
    char curPwd[WIFI_PWD_MAX_LEN + 1] = { 0 };
    EEPROMInterface eeprom;
    eeprom.readWiFiSetting(curSsid, sizeof(curSsid) - 1, curPwd, sizeof(curPwd) - 1);

    Screen.clean();
    Screen.print(0, "EkSkog IoT cfg");
    Screen.print(1, apSsid);
    Screen.print(2, "192.168.0.1");

    TCPServer server;
    if (server.open(WiFiAPInterface()) != 0 || server.bind(80) != 0 || server.listen(1) != 0)
    {
        oledStatus("EkSkog IoT", "listen FAIL", NULL);
        return;
    }

    for (;;)
    {
        TCPSocket client;
        if (server.accept(&client) != 0) continue;
        client.set_timeout(5000);

        char buf[1024];
        int n = client.recv(buf, sizeof(buf) - 1);
        if (n <= 0) { client.close(); continue; }
        buf[n] = '\0';

        String req(buf);
        if (req.startsWith("GET /save?"))
        {
            int sp = req.indexOf(" HTTP");
            String qs = req.substring(10, sp < 0 ? (int)req.length() : sp);

            String ssid = qsGet(qs, "ssid");
            String pwd  = qsGet(qs, "pwd");
            if (ssid.length())
                eeprom.write((uint8_t *)ssid.c_str(), ssid.length() + 1, WIFI_SSID_ZONE_IDX);
            if (pwd.length())   // blank leaves the stored password alone
                eeprom.write((uint8_t *)pwd.c_str(), pwd.length() + 1, WIFI_PWD_ZONE_IDX);

            String cfg = "host=" + qsGet(qs, "host") + ";port=" + qsGet(qs, "port") +
                         ";site=" + qsGet(qs, "site") + ";room=" + qsGet(qs, "room") +
                         ";id=" + qsGet(qs, "id");
            eeprom.write((uint8_t *)cfg.c_str(), cfg.length() + 1, AZ_IOT_HUB_ZONE_IDX);

            sendAndClose(client,
                         String("<body><header><h1 class=\"logo\">EkSkog IoT Settings</h1></header>"
                                "<section class=\"container\"><div id=\"content\" class=\"row\">"
                                "<div class=\"col-sm-10 col-sm-offset-1 col-md-4 col-md-offset-4\">"
                                "<table><tr><th>Settings</th></tr><tr><td>saved</td></tr></table>"
                                "<p style=\"color:#0277bd;\"><b>Rebooting&hellip;</b></p><pre>") +
                         cfg + "</pre></div></div></section></body></html>");
            oledStatus("EkSkog IoT", "config saved", "rebooting");
            wait_ms(1500);
            NVIC_SystemReset();
        }

        sendAndClose(client, portalPage(curSsid));
    }
}

// Called by the core's main() before it decides between configuration mode, AP
// mode and user mode. Taking B here means B+reset opens our portal instead of
// Microsoft's. A+reset is left alone (it still enters the SDK's serial CLI).
void __sys_setup(void)
{
    pinMode(USER_BUTTON_B, INPUT);
    if (digitalRead(USER_BUTTON_B) == LOW)
    {
        runConfigPortal();      // only returns if the AP could not be started
    }
}

static bool connectWiFi()
{
    // Credentials come from EEPROM, written by the config portal above.
    // Never compiled in.
    oledStatus("EkSkog IoT", "WiFi connecting", NULL);
    if (WiFi.begin() != WL_CONNECTED)
    {
        oledStatus("EkSkog IoT", "WiFi FAILED", "B+reset to cfg");
        return false;
    }
    char buf[24];
    snprintf(buf, sizeof(buf), "RSSI %d dBm", WiFi.RSSI());
    oledStatus("EkSkog IoT", WiFi.SSID(), buf);
    return true;
}

static bool connectMQTT()
{
    oledStatus("EkSkog IoT", "MQTT connecting", cfgHost);

    if (mqttClient != NULL) { delete mqttClient; mqttClient = NULL; }
    if (mqttNetwork != NULL) { delete mqttNetwork; mqttNetwork = NULL; }

    mqttNetwork = new MQTTNetwork();
    int rc = mqttNetwork->connect(cfgHost, cfgPort);
    if (rc != 0)
    {
        oledStatus("EkSkog IoT", "MQTT TCP fail", cfgHost);
        return false;
    }

    mqttClient = new MQTT::Client<MQTTNetwork, Countdown, 256>(*mqttNetwork);
    MQTTPacket_connectData options = MQTTPacket_connectData_initializer;
    options.MQTTVersion = 4;  // 3.1.1
    options.clientID.cstring = cfgId;
    options.keepAliveInterval = 60;
    options.cleansession = 1;

    rc = mqttClient->connect(options);
    if (rc != 0)
    {
        oledStatus("EkSkog IoT", "MQTT CONN fail", cfgId);
        return false;
    }
    return true;
}

static bool ensureConnected()
{
    if (WiFi.status() != WL_CONNECTED && !connectWiFi()) return false;
    if (mqttClient == NULL || !mqttClient->isConnected()) return connectMQTT();
    return true;
}

static void printConfig()
{
    Serial.printf("host=%s;port=%d;site=%s;room=%s;id=%s\r\n",
                  cfgHost, cfgPort, cfgSite, cfgRoom, cfgId);
    Serial.printf("topic=%s  source=%s\r\n",
                  cfgTopic, cfgFromEEPROM ? "EEPROM" : "compiled defaults");
    Serial.println("commands:  cfg show | cfg clear | cfg <k=v;k=v...>");
}

// Same settings over the ST-Link serial port (115200), for when a terminal is
// already open and reaching for a phone is not worth it.
static void serialConfigPoll()
{
    static char line[AZ_IOT_HUB_MAX_LEN + 1];
    static size_t len = 0;

    while (Serial.available())
    {
        char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c != '\n')
        {
            if (len < sizeof(line) - 1) line[len++] = c;
            continue;
        }
        line[len] = '\0';
        len = 0;

        if (strncmp(line, "cfg", 3) != 0) { printConfig(); continue; }
        const char *arg = line + 3;
        while (*arg == ' ') arg++;

        if (*arg == '\0' || strcmp(arg, "show") == 0) { printConfig(); continue; }

        EEPROMInterface eeprom;
        if (strcmp(arg, "clear") == 0)
        {
            uint8_t empty = 0;
            eeprom.write(&empty, 1, AZ_IOT_HUB_ZONE_IDX);
            Serial.println("cleared -- rebooting to secrets.h defaults");
        }
        else
        {
            if (strchr(arg, '=') == NULL)
            {
                Serial.println("no k=v pairs found, ignored");
                continue;
            }
            eeprom.write((uint8_t *)arg, strlen(arg) + 1, AZ_IOT_HUB_ZONE_IDX);
            Serial.printf("saved: %s\r\nrebooting\r\n", arg);
        }
        oledStatus("EkSkog IoT", "config saved", "rebooting");
        delay(500);
        NVIC_SystemReset();
    }
}

void setup()
{
    Screen.init();
    Serial.begin(115200);

    loadConfig();
    printConfig();
    oledStatus("EkSkog IoT", cfgId, cfgFromEEPROM ? "cfg: EEPROM" : "cfg: compiled");
    delay(2500);
    oledStatus("EkSkog IoT", cfgTopic, cfgHost);
    delay(2500);
    oledStatus("EkSkog IoT", "B + reset", "= config portal");
    delay(2000);

    oledStatus("EkSkog IoT", "Init sensors...", NULL);

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
    serialConfigPoll();

    if (!ensureConnected())
    {
        oledStatus("EkSkog IoT", "no connection", "B+reset = config");
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

    int rc = mqttClient->publish(cfgTopic, payload, len, MQTT::QOS0);

    char l1[24], l2[24];
    snprintf(l1, sizeof(l1), "T%.1f H%.0f", temperature, humidity);
    snprintf(l2, sizeof(l2), rc == 0 ? "sent P%.0f" : "PUB ERR %d",
             rc == 0 ? pressure : (float)rc);
    oledStatus("EkSkog IoT", l1, l2);

    mqttClient->yield(PUBLISH_INTERVAL_MS);
}
