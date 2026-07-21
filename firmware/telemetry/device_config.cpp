#include "device_config.h"
#include "EEPROMInterface.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>

bool loadDeviceConfig(DeviceConfig &out)
{
    memset(&out, 0, sizeof(out));

    EEPROMInterface eeprom;
    int ret = eeprom.read((uint8_t *)&out, sizeof(out), 0, DEVICE_CONFIG_ZONE);

    if (ret < 0 || out.magic != DEVICE_CONFIG_MAGIC)
    {
        memset(&out, 0, sizeof(out));
        out.mqttPort = 1883;
        return false;
    }
    return true;
}

bool saveDeviceConfig(DeviceConfig &cfg)
{
    cfg.magic = DEVICE_CONFIG_MAGIC;
    EEPROMInterface eeprom;
    int ret = eeprom.write((uint8_t *)&cfg, sizeof(cfg), DEVICE_CONFIG_ZONE);
    return ret >= 0;
}

void buildClientId(const char *site, const char *room, char *out, size_t outSize)
{
    snprintf(out, outSize, "az3166-%s-%s", site, room);
    for (char *p = out; *p; p++)
    {
        *p = (char)tolower((unsigned char)*p);
    }
}
