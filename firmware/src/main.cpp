// WiFi-only diagnostic — isolates the AZ3166 Wi-Fi stack (no MQTT, no sensors).
// Mirrors the framework's ConnectWithWPA example. Prints each step over serial
// and to the OLED so we can see exactly how far boot gets before any crash.

#include "Arduino.h"
#include "OledDisplay.h"
#include "AZ3166WiFi.h"
#include "secrets.h"

void setup()
{
    Serial.begin(115200);
    Screen.init();
    Screen.clean();
    Screen.print(0, "WiFi test");
    Serial.println("=== AZ3166 WiFi-only diagnostic ===");

    if (WiFi.status() == WL_NO_SHIELD)
    {
        Serial.println("WiFi shield not present");
        Screen.print(1, "NO SHIELD");
        while (true) {}
    }

    const char *fv = WiFi.firmwareVersion();
    Serial.printf("WiFi firmware version: %s\r\n", fv);

    Screen.print(1, "Connecting...");
    Serial.printf("Connecting to SSID '%s'...\r\n", WIFI_SSID);

    int status = WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.printf("WiFi.begin() returned: %d\r\n", status);

    if (status == WL_CONNECTED)
    {
        IPAddress ip = WiFi.localIP();
        Serial.print("Connected! IP: ");
        Serial.println(ip);
        Serial.printf("RSSI: %d dBm\r\n", WiFi.RSSI());

        Screen.clean();
        Screen.print(0, "WiFi test");
        Screen.print(1, "CONNECTED");
        char buf[24];
        snprintf(buf, sizeof(buf), "RSSI %d dBm", WiFi.RSSI());
        Screen.print(2, buf);
    }
    else
    {
        Serial.println("WiFi connect FAILED");
        Screen.print(2, "FAILED");
    }
}

void loop()
{
    Serial.printf("alive: status=%d rssi=%d\r\n", WiFi.status(), WiFi.RSSI());
    delay(5000);
}
