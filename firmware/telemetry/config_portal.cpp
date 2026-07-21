// Page look (CSS, layout, "IoT DevKit Settings" chrome) is lifted verbatim
// from this core's own cores/arduino/httpserver/app_httpd.cpp — same
// mini.css v2.1.5 (Angelos Chalaris, MIT) that Microsoft's original
// AZ IoT Hub config page used, restyled around this project's own fields.
#include "config_portal.h"
#include "OledDisplay.h"
#include "AZ3166WiFi.h"
#include "mico_wlan.h" // micoWlanGetIPStatus(): WiFi.localIP() reflects the
                        // *station* interface and reads 0.0.0.0 in pure AP
                        // mode (the core's own AP example never calls it
                        // either) — this is the real, lower-level way to read
                        // the SoftAP's own address.
#include "SystemWiFi.h" // WiFiScan(): nearby-network list for the SSID field
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define MAX_SCAN_RESULTS 15
static WiFiAccessPoint scanResults[MAX_SCAN_RESULTS];
static int scanCount = 0;

// Scanning is a station-radio operation; it needs to happen before beginAP()
// switches the interface over to Soft_AP, not after.
static void scanNetworks()
{
    Serial.println("Scanning for nearby Wi-Fi networks...");
    scanCount = WiFiScan(scanResults, MAX_SCAN_RESULTS);
    if (scanCount < 0) scanCount = 0;
    Serial.printf("Found %d network(s)\r\n", scanCount);
}

static void buildScanOptionsHtml(char *out, size_t outSize)
{
    out[0] = '\0';
    size_t used = 0;
    for (int i = 0; i < scanCount; i++)
    {
        const char *ssid = scanResults[i].get_ssid();
        if (!ssid || !ssid[0]) continue;
        int n = snprintf(out + used, outSize - used, "<option value=\"%s\">", ssid);
        if (n < 0 || (size_t)n >= outSize - used) break;
        used += n;
    }
}

// ponytail: fixed SSID, no per-board MAC suffix. An admin configures one
// board at a time standing next to it, so a shared SSID is fine; add a MAC
// suffix (WiFi.macAddress()) if boards ever need simultaneous setup.
static const char *AP_SSID = "AZ3166-setup";
// This core's SystemWiFiAPStart() hardcodes NSAPI_SECURITY_WPA_WPA2 for the
// SoftAP regardless of what passphrase is passed — an empty passphrase (the
// legacy example's "open" AP) lets clients associate at the link layer but
// never completes a WPA2 handshake, so the interface never finishes bringing
// up its IP stack (localIP() sticks at 0.0.0.0). A real WPA2 passphrase is
// mandatory here, not optional.
static const char *AP_PASSWORD = "az3166setup";
static const uint32_t PORTAL_TIMEOUT_MS = 10UL * 60UL * 1000UL; // reboot if nobody shows up
static const uint32_t CLIENT_TIMEOUT_MS = 5000;                 // per-request read timeout
static const uint32_t AP_IP_WAIT_MS = 15000;                    // wait for the AP's IP to come up

static void oledPortalStatus(const char *ip)
{
    Screen.clean();
    Screen.print(0, AP_SSID);
    Screen.print(1, AP_PASSWORD);
    Screen.print(2, ip);
}

// Reads a full HTTP request (headers + body, if Content-Length says there
// is one) off `client`, character at a time — this core's WiFiClient has no
// Stream-style readStringUntil, only read()/available() (see the core's own
// WiFiWebServer.ino example).
static String readRequest(WiFiClient &client)
{
    String req;
    uint32_t lastByte = millis();
    int bodyStart = -1;
    int contentLength = -1;

    while (millis() - lastByte < CLIENT_TIMEOUT_MS)
    {
        while (client.available())
        {
            char c = client.read();
            req += c;
            lastByte = millis();

            if (bodyStart < 0 && req.endsWith("\r\n\r\n"))
            {
                bodyStart = req.length();
                int idx = req.indexOf("Content-Length:");
                if (idx >= 0) contentLength = req.substring(idx + 15).toInt();
                if (contentLength <= 0) return req;
            }
            if (bodyStart >= 0 && (int)req.length() - bodyStart >= contentLength)
            {
                return req;
            }
        }
    }
    return req;
}

static String urlDecode(const String &in)
{
    String out;
    for (unsigned int i = 0; i < in.length(); i++)
    {
        char c = in[i];
        if (c == '+')
        {
            out += ' ';
        }
        else if (c == '%' && i + 2 < in.length())
        {
            char hex[3] = { in[i + 1], in[i + 2], 0 };
            out += (char)strtol(hex, NULL, 16);
            i += 2;
        }
        else
        {
            out += c;
        }
    }
    return out;
}

static String formField(const String &body, const char *key)
{
    String needle = String(key) + "=";
    int idx = body.indexOf(needle);
    if (idx < 0) return "";
    unsigned int start = idx + needle.length();
    int end = body.indexOf('&', start);
    if (end < 0) end = body.length();
    return urlDecode(body.substring(start, end));
}

// Verbatim mini.css (v2.1.5 "Fermion", Angelos Chalaris, MIT) plus page
// chrome, lifted from this core's own cores/arduino/httpserver/app_httpd.cpp
// ("IoT DevKit Settings") — sent as-is via client.print(), not through
// snprintf, so the CSS's literal '%' (percentage widths) need no escaping.
static const char *PAGE_HEAD =
    "<!doctype html><html lang=\"en\"><head><meta charset=\"UTF-8\">"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
    "<title>AZ3166 WiFi Config</title><style>html{font-size:16px;}html, *{font-family:-apple-system, BlinkMacSystemFont,\"Segoe UI\",\"Roboto\", \"Droid Sans\",\"Helvetica Neue\", Helvetica, Arial, sans-serif;line-height:1.5;-webkit-text-size-adjust:100%;}*{font-size:1rem;}body{margin:0;color:#212121;background:#f8f8f8;}section{display:block;}input{overflow:visible;}h1{font-size:2rem;line-height:1.2em;margin:0.75rem 0.5rem;font-weight:500;}.container{margin:0 auto;padding:0 0.75rem;}.row{box-sizing:border-box;display:flex;flex:0 1 auto;flex-flow:row wrap;}[class^='col-sm-']{box-sizing:border-box;flex:0 0 auto;padding:0 0.25rem;}.col-sm-10{max-width:83.33333%;flex-basis:83.33333%;}.col-sm-offset-1{margin-left:8.33333%;}@media screen and (min-width:768px){.col-md-4{max-width:33.33333%;flex-basis:33.33333%;}.col-md-offset-4{margin-left:33.33333%;}}header{display:block;height:2.75rem;background:#1e6bb8;color:#f5f5f5;padding:0.125rem 0.5rem;white-space:nowrap;overflow-x:auto;overflow-y:hidden;}header .logo{color:#f5f5f5;font-size:1.35rem;line-height:1.8125em;margin:0.0625rem 0.375rem 0.0625rem 0.0625rem;text-decoration:none;}form{background:#eeeeee;border:1px solid #c9c9c9;margin:0.5rem;padding:0.75rem 0.5rem 1.125rem;}.input-group.fluid{display:flex;align-items:center;justify-content:center;}.input-group.fluid>input{flex-grow:1;flex-basis:0;}@media screen and (max-width:767px){.input-group.fluid{align-items:stretch;flex-direction:column;}}[type=\"password\"],[type=\"text\"],[type=\"number\"],select{width:100%;box-sizing:border-box;background:#fafafa;color:#212121;border:1px solid #c9c9c9;border-radius:2px;margin:0.25rem 0;padding:0.5rem 0.75rem;}input:hover, input:focus, select:hover, select:focus{border-color:#0288d1;box-shadow:none;}::placeholder{opacity:1;color:#616161;}button, [type=\"submit\"]{display:inline-block;background:rgba(208, 208, 208, 0.75);color:#212121;border:0;border-radius:2px;padding:0.5rem 0.75rem;margin:0.5rem;text-decoration:none;cursor:pointer;}button.primary, [type=\"submit\"].primary{background:rgba(30, 107, 184, 0.9);color:#fafafa;}button.primary:hover, [type=\"submit\"].primary:hover{background:#0277bd;}#content{margin-top:2em;}</style></head>"
    "<body><header><h1 class=\"logo\">IoT DevKit Settings</h1></header>"
    "<section class=\"container\"><div id=\"content\" class=\"row\">"
    "<div class=\"col-sm-10 col-sm-offset-1 col-md-4 col-md-offset-4\" style=\"text-align:center;\">";

static const char *PAGE_END = "</div></div></section></body></html>";

static void sendPage(WiFiClient &client, const DeviceConfig &current, const char *notice)
{
    // static, not stack locals: this sketch's RTOS thread stack is small
    // enough that a couple-KB frame here plausibly overflowed it, silently
    // resetting the board mid-portal.
    static char options[1024];
    buildScanOptionsHtml(options, sizeof(options));

    static char form[2560];
    snprintf(form, sizeof(form),
        "%s"
        "<form method=\"POST\" action=\"/save\">"
        "<div><fieldset><legend>Wi-Fi Settings</legend>"
        "<div class=\"input-group fluid\"><input type=\"text\" name=\"ssid\" list=\"nets\" placeholder=\"SSID\" value=\"%s\" required>"
        "<datalist id=\"nets\">%s</datalist></div>"
        "<div class=\"input-group fluid\"><input type=\"password\" name=\"pass\" placeholder=\"Password\" value=\"%s\"></div>"
        "</fieldset></div>"
        "<div><fieldset><legend>MQTT Broker Settings</legend>"
        "<div class=\"input-group fluid\"><input type=\"text\" name=\"host\" placeholder=\"Broker host\" value=\"%s\" required></div>"
        "<div class=\"input-group fluid\"><input type=\"number\" name=\"port\" placeholder=\"Broker port\" value=\"%u\" required></div>"
        "</fieldset></div>"
        "<div><fieldset><legend>Location</legend>"
        "<div class=\"input-group fluid\"><input type=\"text\" name=\"site\" placeholder=\"Site\" value=\"%s\" required></div>"
        "<div class=\"input-group fluid\"><input type=\"text\" name=\"room\" placeholder=\"Room\" value=\"%s\" required></div>"
        "</fieldset></div>"
        "<div class=\"input-group fluid\"><button type=\"submit\" class=\"primary\">Configure Device</button></div>"
        "</form>",
        notice ? notice : "",
        current.wifiSsid, options, current.wifiPassword, current.mqttHost,
        (unsigned)(current.mqttPort ? current.mqttPort : 1883), current.site, current.room);

    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/html");
    client.println("Connection: close");
    client.println();
    client.print(PAGE_HEAD);
    client.print(form);
    client.print(PAGE_END);
}

static void handleSave(WiFiClient &client, const String &body, const DeviceConfig &current)
{
    String ssid = formField(body, "ssid");
    String pass = formField(body, "pass");
    String host = formField(body, "host");
    String port = formField(body, "port");
    String site = formField(body, "site");
    String room = formField(body, "room");

    if (!ssid.length() || !host.length() || !site.length() || !room.length())
    {
        sendPage(client, current, "<h5 style=\"color:Tomato;\">All fields except password are required.</h5>");
        return;
    }

    DeviceConfig next;
    memset(&next, 0, sizeof(next));
    strncpy(next.wifiSsid, ssid.c_str(), sizeof(next.wifiSsid) - 1);
    strncpy(next.wifiPassword, pass.c_str(), sizeof(next.wifiPassword) - 1);
    strncpy(next.mqttHost, host.c_str(), sizeof(next.mqttHost) - 1);
    next.mqttPort = port.length() ? (uint16_t)port.toInt() : 1883;
    strncpy(next.site, site.c_str(), sizeof(next.site) - 1);
    strncpy(next.room, room.c_str(), sizeof(next.room) - 1);
    saveDeviceConfig(next);

    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/html");
    client.println("Connection: close");
    client.println();
    client.print(PAGE_HEAD);
    client.print("<h5 style=\"color:DodgerBlue;\">The IoT DevKit is rebooting...</h5>");
    client.print(PAGE_END);
    client.flush();
    delay(200);
    NVIC_SystemReset();
}

void runConfigPortal(const DeviceConfig &startConfig)
{
    DeviceConfig current = startConfig;

    Serial.println("Entering setup mode...");
    oledPortalStatus("starting...");
    scanNetworks(); // must run before beginAP() switches the radio to Soft_AP
    WiFi.beginAP((char *)AP_SSID, (char *)AP_PASSWORD);

    char ipStr[16] = "starting...";
    IPStatusTypedef ipStatus;
    memset(&ipStatus, 0, sizeof(ipStatus));
    uint32_t ipWaitStart = millis();
    do
    {
        if (micoWlanGetIPStatus(&ipStatus, Soft_AP) == 0 &&
            ipStatus.ip[0] && strcmp(ipStatus.ip, "0.0.0.0") != 0)
        {
            strncpy(ipStr, ipStatus.ip, sizeof(ipStr) - 1);
            break;
        }
        delay(200);
    } while (millis() - ipWaitStart < AP_IP_WAIT_MS);

    Serial.printf("AP '%s' up, browse to http://%s/\r\n", AP_SSID, ipStr);
    oledPortalStatus(ipStr);

    WiFiServer server(80);
    server.begin();

    uint32_t portalStart = millis();
    while (millis() - portalStart < PORTAL_TIMEOUT_MS)
    {
        WiFiClient client = server.available();
        if (!client)
        {
            delay(50);
            continue;
        }

        String req = readRequest(client);
        int bodyIdx = req.indexOf("\r\n\r\n");
        String body = bodyIdx >= 0 ? req.substring(bodyIdx + 4) : "";

        if (req.startsWith("POST /save"))
        {
            handleSave(client, body, current); // reboots on success, falls through on validation error
        }
        else
        {
            sendPage(client, current, NULL);
        }

        client.stop();
        portalStart = millis(); // any interaction resets the idle timer
    }

    Serial.println("Setup mode timed out, rebooting...");
    NVIC_SystemReset();
}
