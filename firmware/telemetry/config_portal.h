// Wi-Fi/MQTT setup portal: SoftAP + a tiny HTTP form, replacing the
// legacy MXChip DevKit config-mode (which was Azure-IoT-Hub-shaped) with
// this project's own schema (MQTT host/port + site/room).
#ifndef CONFIG_PORTAL_H
#define CONFIG_PORTAL_H

#include "device_config.h"

// Brings up "AZ3166-setup", serves the form at http://<ap-ip>/, and saves
// + reboots on submit. Also reboots on its own after an idle timeout so an
// auto-entered portal (no admin nearby) doesn't broadcast forever. Never
// returns.
void runConfigPortal(const DeviceConfig &current);

#endif // CONFIG_PORTAL_H
