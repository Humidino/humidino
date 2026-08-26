#include "wifi_provision.h"

#include <WiFiManager.h>

#include "config.h"

namespace WifiProvision {

void begin() {
    WiFiManager wm;
    wm.setConfigPortalTimeout(180);
    wm.autoConnect(DEVICE_HOSTNAME);
}

}  // namespace WifiProvision
