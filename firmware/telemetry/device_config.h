// Device identity, persisted on the STSAFE-A100 secure element via
// EEPROMInterface — not internal flash, so this can't collide with the app
// image (0x0800C000) or bootloader (0x08000000). Replaces compile-time
// secrets.h: a board with no valid config here boots into the setup portal
// instead (see config_portal.h).
#ifndef DEVICE_CONFIG_H
#define DEVICE_CONFIG_H

#include <stdint.h>
#include <stddef.h>

#define DEVICE_CONFIG_MAGIC 0xC0FFEE01u
// STSAFE_ZONE_7_IDX — one of the two zones EEPROMInterface.h documents as
// free for app use (the other, zone 8, is left free for later use, e.g. a
// temperature calibration offset).
#define DEVICE_CONFIG_ZONE 7

struct DeviceConfig
{
    uint32_t magic;
    char wifiSsid[33];
    char wifiPassword[64];
    char mqttHost[64];
    uint16_t mqttPort;
    char site[24];
    char room[24];
};

// Reads the config zone. Returns true if it held a valid (magic-matched)
// config; on false, `out` is zeroed with mqttPort defaulted to 1883.
bool loadDeviceConfig(DeviceConfig &out);

// Stamps the magic and persists `cfg` to the config zone.
bool saveDeviceConfig(DeviceConfig &cfg);

// Derives the MQTT client ID from site/room (lowercased) so two boards can
// never collide on a hand-typed client ID the way secrets.h allowed.
void buildClientId(const char *site, const char *room, char *out, size_t outSize);

#endif // DEVICE_CONFIG_H
