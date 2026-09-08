#include "wifi_provision.h"

#include <WiFiManager.h>

#include "config.h"

namespace WifiProvision {

bool begin() {
    WiFiManager wm;
    wm.setConfigPortalTimeout(180);
    return wm.autoConnect(DEVICE_HOSTNAME);
}

void forgetCredentials() {
    WiFiManager wm;
    wm.resetSettings();
}

}  // namespace WifiProvision
