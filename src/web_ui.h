#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include "config.h"
#include "web_ui_assets.h"

namespace WebUi {

struct PortalState {
    String savedSSID;
    String savedEapIdentity;
    bool hasApiKey = false;
    bool debugApEnabled = DEFAULT_DEBUG_AP_ENABLED;
    uint8_t cameraProfile = CAM_PROFILE_DEFAULT;
    bool photoRecapEnabled = DEFAULT_PHOTO_RECAP_ENABLED;
};

// Served by configServer's "/" handler -- the React config portal app.
inline const uint8_t *configPageGzip() { return CONFIG_PAGE_GZIP; }
inline size_t configPageGzipLen() { return CONFIG_PAGE_GZIP_LEN; }

// Served by debugServer's "/" handler -- the React camera debug app.
inline const uint8_t *debugPageGzip() { return DEBUG_PAGE_GZIP; }
inline size_t debugPageGzipLen() { return DEBUG_PAGE_GZIP_LEN; }

// Consumed by the config app's `fetch('/api/config')` on load.
String renderConfigJson(const PortalState &state) {
    StaticJsonDocument<384> doc;
    doc["ssid"] = state.savedSSID;
    doc["eapIdentity"] = state.savedEapIdentity;
    doc["hasApiKey"] = state.hasApiKey;
    doc["debugApEnabled"] = state.debugApEnabled;
    doc["cameraProfile"] = normalizeCameraProfileValue(state.cameraProfile);
    doc["photoRecapEnabled"] = state.photoRecapEnabled;

    String json;
    serializeJson(doc, json);
    return json;
}

}  // namespace WebUi
