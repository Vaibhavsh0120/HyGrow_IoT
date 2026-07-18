// ----------------------------------------------------------------------------
// command_handlers.cpp — every WebSocket "command" branch + pin validation.
// ----------------------------------------------------------------------------
// Split out of the original task_network.cpp (see task_network_internal.h
// for the full map of the split). Owns:
//   - sendCmdAck(): the per-command ack every save/calibrate button in
//     app.js's sendCommand() waits on before showing "Saved!"
//   - isForbiddenPin()/validatePinSet(): the server-side pin safety net
//     behind the client-side check in Settings (app.js) — the real boundary,
//     since a hand-crafted WS message can skip the browser entirely
//   - handleDeviceCommand(): every command name other than "auth"/
//     "change_password" (those live in auth.cpp) — save_wifi, save_firebase,
//     save_pins, calibrate_ph, calibrate_tds, save_features,
//     save_sensor_enabled, save_intervals, reset_sensor_pin, factory_reset,
//     reboot, request_vitals
//
// Structural split from the original file, PLUS new input validation that
// didn't exist before:
//   - save_wifi rejects an empty SSID (a device can't usefully "connect" to
//     no network — this used to silently save "" and only surface as a
//     confusing SoftAP fallback ~15s after the next reboot).
//   - save_firebase rejects a Project ID containing anything other than
//     lowercase letters, digits, and hyphens (Firebase's own project ID
//     rules), so a typo'd/garbage ID fails fast in the Settings form instead
//     of failing silently on every future Firestore PATCH.
//   - calibrate_tds rejects a target ppm outside a realistic range for this
//     sensor/board (0–10000 ppm covers tap water through concentrated
//     hydroponic nutrient solution and standard TDS calibration fluids;
//     anything past that, including negative values, is either a typo or a
//     unit mistake, not a real calibration fluid).
// ----------------------------------------------------------------------------
#include "task_network.h"
#include "task_network_internal.h"
#include "state.h"

// Sends a per-command acknowledgement directly to the requesting client only
// (never broadcast) — the frontend's real-ack save flow (app.js sendCommand())
// waits for exactly this frame before showing "Saved!", instead of assuming
// success the instant websocket.send() returns. `cmd` echoes back the command
// name so the client can match the ack to the button that sent it even if
// several saves are in flight at once; `error` is optional context for a
// rejected command (e.g. pin validation failure) and is omitted when ok.
void sendCmdAck(AsyncWebSocketClient *client, const String &cmd, bool ok, const String &error /* = "" */)
{
    JsonDocument resp;
    resp["type"] = "command_result";
    resp["command"] = cmd;
    resp["ok"] = ok;
    if (!ok && error.length() > 0)
        resp["error"] = error;
    String payload;
    serializeJson(resp, payload);
    client->text(payload);
}

// ----------------------------------------------------------------------------
// Server-side pin validation (Part 5.4 / 5.5)
// ----------------------------------------------------------------------------
// The client-side check in Settings (app.js) is a UX nicety only — it can be
// bypassed by a hand-crafted WS message. These two helpers are the real
// safety boundary between "the browser sent something" and "it lands in
// currentConfig/NVS". Neither one is a substitute for enforceForbiddenPins()
// in HyGrow_IoT.ino, which remains the last-resort boot-time guard.

// True if `pin` is a reserved USB D-/D+ line. -1 (disabled) is always fine.
static bool isForbiddenPin(int pin)
{
    return pin == 19 || pin == 20;
}

// Checks a proposed full set of sensor pins for GPIO19/20 use and for
// duplicate assignments between two different *enabled* sensors. -1 never
// conflicts with anything. Returns "" if the whole set is valid, or a
// human-readable reason naming the offending sensor(s)/pin if not.
struct PinCheckEntry
{
    int pin;
    const char *label;
};
static String validatePinSet(PinCheckEntry entries[], int count)
{
    for (int i = 0; i < count; i++)
    {
        if (isForbiddenPin(entries[i].pin))
        {
            return String(entries[i].label) + " is set to GPIO" + String(entries[i].pin) +
                   ", which is reserved for USB on this board.";
        }
    }

    for (int i = 0; i < count; i++)
    {
        if (entries[i].pin < 0)
            continue;
        for (int j = i + 1; j < count; j++)
        {
            if (entries[j].pin < 0)
                continue;
            if (entries[i].pin == entries[j].pin)
            {
                return String(entries[i].label) + " and " + String(entries[j].label) +
                       " are both assigned to GPIO" + String(entries[i].pin) + ".";
            }
        }
    }

    return "";
}

// ----------------------------------------------------------------------------
// Form-input validation (new)
// ----------------------------------------------------------------------------

// Firebase/Firestore project IDs are always lowercase letters, digits, and
// hyphens, 6-30 characters, and can't start/end with a hyphen (Google's own
// project ID rules). Rejecting anything else here fails fast in the
// Settings form instead of failing silently on every future Firestore PATCH
// (firebaseUploadCycle(), firebase.cpp) with an opaque HTTP error.
static bool isValidFirebaseProjectId(const String &projectId)
{
    int len = projectId.length();
    if (len < 6 || len > 30)
        return false;
    if (projectId.charAt(0) == '-' || projectId.charAt(len - 1) == '-')
        return false;

    for (int i = 0; i < len; i++)
    {
        char c = projectId.charAt(i);
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok)
            return false;
    }
    return true;
}

// Realistic bounds for a TDS calibration-fluid target: 0 ppm (pure/deionized
// water) through 10000 ppm comfortably covers tap water, standard TDS
// calibration solutions (typically 342/707/1382/1500 ppm scale, or up to
// ~12880 ppm on the 442-scale for the strongest commercial fluids), and
// concentrated hydroponic nutrient reservoirs. Anything outside that —
// negative values or something like 999999 — is a typo or unit mistake, not
// a real calibration fluid, and would silently wreck every future TDS
// reading via the tds_k scale factor (see sensor_tds.cpp).
static bool isRealisticTdsPpm(float ppm)
{
    return !isnan(ppm) && ppm >= 0.0f && ppm <= 10000.0f;
}

// Every command name other than "auth"/"change_password" (those live in
// auth.cpp) — save_wifi, save_firebase, save_pins, calibrate_ph,
// calibrate_tds, save_features, save_sensor_enabled, save_intervals,
// reset_sensor_pin, factory_reset, reboot, request_vitals. Only ever called
// once the client has passed auth (see the gate in handleWebSocketMessage(),
// websocket.cpp).
void handleDeviceCommand(AsyncWebSocketClient *client, const String &cmd, JsonDocument &doc)
{
    if (cmd == "save_wifi")
    {
        String ssid = doc["ssid"] | "";
        // An empty SSID can't usefully be connected to — this used to save
        // silently and only surface ~15s after the next reboot as a
        // confusing SoftAP fallback with no explanation on this screen.
        // Reject it here, before it ever reaches currentConfig/NVS.
        if (ssid.length() == 0)
        {
            webLog(0, LOG_ERR, "save_wifi rejected: SSID cannot be empty.");
            sendCmdAck(client, cmd, false, "Wi-Fi network name (SSID) cannot be empty.");
            return;
        }

        strlcpy(currentConfig.wifi_ssid, ssid.c_str(), sizeof(currentConfig.wifi_ssid));
        strlcpy(currentConfig.wifi_pass, doc["pass"] | "", sizeof(currentConfig.wifi_pass));
        if (!state_save())
        {
            webLog(0, LOG_ERR, "save_wifi: state_save() failed — settings may not be fully persisted.");
            sendCmdAck(client, cmd, false, "Failed to save. Device storage may be full or corrupted.");
            return;
        }
        broadcastConfig();
        webLog(0, LOG_INFO, "WiFi config saved. Reboot to apply.");
        sendCmdAck(client, cmd, true);
    }
    else if (cmd == "save_firebase")
    {
        String projectId = doc["proj"] | "";
        // Only validate a non-empty project ID — an empty one is how a user
        // clears/disables Firebase provisioning, which stays allowed (the
        // upload cycle already no-ops on an empty fb_project, see
        // firebaseUploadCycle() in firebase.cpp).
        if (projectId.length() > 0 && !isValidFirebaseProjectId(projectId))
        {
            webLog(0, LOG_ERR, "save_firebase rejected: invalid Project ID '" + projectId + "'.");
            sendCmdAck(client, cmd, false, "Invalid Firebase Project ID. Use 6-30 lowercase letters, digits, or hyphens.");
            return;
        }

        strlcpy(currentConfig.fb_api_key, doc["api"] | "", sizeof(currentConfig.fb_api_key));
        strlcpy(currentConfig.fb_project, projectId.c_str(), sizeof(currentConfig.fb_project));
        strlcpy(currentConfig.fb_email, doc["email"] | "", sizeof(currentConfig.fb_email));
        strlcpy(currentConfig.fb_pass, doc["pass"] | "", sizeof(currentConfig.fb_pass));
        strlcpy(currentConfig.fb_collection, doc["col"] | "", sizeof(currentConfig.fb_collection));
        if (!state_save())
        {
            webLog(0, LOG_ERR, "save_firebase: state_save() failed — settings may not be fully persisted.");
            sendCmdAck(client, cmd, false, "Failed to save. Device storage may be full or corrupted.");
            return;
        }
        broadcastConfig();
        webLog(0, LOG_INFO, "Firebase config updated.");
        sendCmdAck(client, cmd, true);
    }
    else if (cmd == "save_pins")
    {
        // Build the proposed post-apply pin set first, so we can validate
        // it as a whole (forbidden pins + cross-sensor duplicates) BEFORE
        // committing anything to currentConfig/NVS. Reject the whole
        // command if anything is wrong — same "all or nothing" behavior
        // save_pins already had, just with a validation gate in front.
        int proposedTds = doc["pin_tds"] | currentConfig.pin_tds;
        int proposedDht = doc["pin_dht"] | currentConfig.pin_dht;
        int proposedPh = doc["pin_ph"] | currentConfig.pin_ph;
        int proposedWt = doc["pin_wt"] | currentConfig.pin_ds18b20;
        int proposedWl = doc["pin_wl"] | currentConfig.pin_wl;
        int proposedSda = doc["pin_sda"] | currentConfig.pin_lux_sda;
        int proposedScl = doc["pin_scl"] | currentConfig.pin_lux_scl;
        int proposedWlp = doc["pin_wlp"] | currentConfig.pin_wl_power;

        PinCheckEntry proposed[] = {
            {proposedTds, "TDS"},
            {proposedDht, "DHT22"},
            {proposedPh, "pH"},
            {proposedWt, "DS18B20 (Water Temp)"},
            {proposedWl, "Water Level Signal"},
            {proposedSda, "BH1750 SDA"},
            {proposedScl, "BH1750 SCL"},
            {proposedWlp, "Water Level Power"},
        };
        String problem = validatePinSet(proposed, 8);

        if (problem.length() > 0)
        {
            webLog(0, LOG_ERR, "save_pins rejected: " + problem);
            sendCmdAck(client, cmd, false, problem);
            return;
        }

        currentConfig.pin_tds = proposedTds;
        currentConfig.pin_dht = proposedDht;
        currentConfig.pin_ph = proposedPh;
        currentConfig.pin_ds18b20 = proposedWt;
        currentConfig.pin_wl = proposedWl;
        currentConfig.pin_lux_sda = proposedSda;
        currentConfig.pin_lux_scl = proposedScl;
        currentConfig.pin_wl_power = proposedWlp;
        if (!state_save())
        {
            webLog(0, LOG_ERR, "save_pins: state_save() failed — settings may not be fully persisted.");
            sendCmdAck(client, cmd, false, "Failed to save. Device storage may be full or corrupted.");
            return;
        }
        broadcastConfig();
        webLog(0, LOG_INFO, "Pinout config saved. Reboot required.");
        sendCmdAck(client, cmd, true);
    }
    else if (cmd == "calibrate_ph")
    {
        currentConfig.ph_offset = doc["offset"] | currentConfig.ph_offset;
        currentConfig.ph_slope = doc["slope"] | currentConfig.ph_slope;
        if (!state_save())
        {
            webLog(0, LOG_ERR, "calibrate_ph: state_save() failed — calibration may not be fully persisted.");
            sendCmdAck(client, cmd, false, "Failed to save. Device storage may be full or corrupted.");
            return;
        }
        broadcastConfig();
        webLog(0, LOG_INFO, "pH Calibration saved.");
        sendCmdAck(client, cmd, true);
    }
    else if (cmd == "calibrate_tds")
    {
        float proposedK = doc["tds_k"] | currentConfig.tds_k;
        // The client sends a K-factor, already computed from
        // target_ppm/current_ppm (see the pH wizard's TDS card in app.js) —
        // but also send the raw target ppm it computed from, so the server
        // can reject an impossible target instead of only ever seeing the
        // derived multiplier. Missing target_ppm (an older client, or a
        // hand-crafted message) skips this specific check and falls back to
        // just persisting the K-factor, same as before.
        if (doc["target_ppm"].is<float>() || doc["target_ppm"].is<int>())
        {
            float targetPpm = doc["target_ppm"];
            if (!isRealisticTdsPpm(targetPpm))
            {
                webLog(0, LOG_ERR, "calibrate_tds rejected: unrealistic target " + String(targetPpm) + " ppm.");
                sendCmdAck(client, cmd, false, "TDS target must be between 0 and 10000 ppm.");
                return;
            }
        }

        // Guard the K-factor itself too — even without target_ppm, a K well
        // outside this range means every future reading gets scaled into
        // nonsense (see the tds_k multiply in readTDS(), sensor_tds.cpp).
        if (isnan(proposedK) || proposedK <= 0.0f || proposedK > 100.0f)
        {
            webLog(0, LOG_ERR, "calibrate_tds rejected: unrealistic K-factor " + String(proposedK) + ".");
            sendCmdAck(client, cmd, false, "Calibration produced an unrealistic scale factor. Check the live reading and target ppm.");
            return;
        }

        currentConfig.tds_k = proposedK;
        if (!state_save())
        {
            webLog(0, LOG_ERR, "calibrate_tds: state_save() failed — calibration may not be fully persisted.");
            sendCmdAck(client, cmd, false, "Failed to save. Device storage may be full or corrupted.");
            return;
        }
        broadcastConfig();
        webLog(0, LOG_INFO, "TDS Calibration saved.");
        sendCmdAck(client, cmd, true);
    }
    else if (cmd == "save_features")
    {
        // Any field the client omits leaves that flag unchanged — same
        // "omit-to-leave-unchanged" pattern used by save_pins. Neither
        // flag requires a reboot: both demo_mode and firebase_enabled
        // are checked live, every cycle.
        currentConfig.demo_mode = doc["demo"] | currentConfig.demo_mode;
        currentConfig.firebase_enabled = doc["fb_en"] | currentConfig.firebase_enabled;
        if (!state_save())
        {
            webLog(0, LOG_ERR, "save_features: state_save() failed — settings may not be fully persisted.");
            sendCmdAck(client, cmd, false, "Failed to save. Device storage may be full or corrupted.");
            return;
        }
        broadcastConfig();
        webLog(0, LOG_INFO, "Feature flags updated.");
        sendCmdAck(client, cmd, true);
    }
    else if (cmd == "save_sensor_enabled")
    {
        // Closes the Part 5.1 bug: today there's no way to flip
        // sensor_enabled[i] back to true from the UI once it's false.
        // Requires a reboot afterward (same as save_pins) since a
        // restored pin only takes effect after the sensor task
        // re-inits at boot.
        String sensor = doc["sensor"] | "";
        bool enabled = doc["enabled"] | true;
        SensorID id = S_COUNT; // sentinel; only used after matched is confirmed true
        bool matched = true;

        if (sensor == "tds")
            id = S_TDS;
        else if (sensor == "dht")
            id = S_DHT;
        else if (sensor == "ph")
            id = S_PH;
        else if (sensor == "wt")
            id = S_WTEMP;
        else if (sensor == "wl")
            id = S_WL;
        else if (sensor == "light")
            id = S_LIGHT;
        else
            matched = false;

        if (!matched)
        {
            webLog(0, LOG_ERR, "save_sensor_enabled: unknown sensor id '" + sensor + "'");
            sendCmdAck(client, cmd, false, "Unknown sensor id '" + sensor + "'");
            return;
        }

        currentConfig.sensor_enabled[id] = enabled;

        // If re-enabling and the pin(s) are still -1 (e.g. from an
        // earlier auto-disable or manual pin reset), restore the
        // compiled default(s) too — otherwise the user enables a
        // sensor whose pins are still -1 and nothing happens. Reuses
        // the same defaults reset_sensor_pin() already applies.
        if (enabled)
        {
            switch (id)
            {
            case S_TDS:
                if (currentConfig.pin_tds < 0)
                    currentConfig.pin_tds = PIN_TDS;
                break;
            case S_DHT:
                if (currentConfig.pin_dht < 0)
                    currentConfig.pin_dht = PIN_DHT;
                break;
            case S_PH:
                if (currentConfig.pin_ph < 0)
                    currentConfig.pin_ph = PIN_PH;
                break;
            case S_WTEMP:
                if (currentConfig.pin_ds18b20 < 0)
                    currentConfig.pin_ds18b20 = PIN_DS18B20;
                break;
            case S_WL:
                if (currentConfig.pin_wl < 0)
                    currentConfig.pin_wl = PIN_WL;
                if (currentConfig.pin_wl_power < 0)
                    currentConfig.pin_wl_power = PIN_WL_PWR;
                break;
            case S_LIGHT:
                if (currentConfig.pin_lux_sda < 0)
                    currentConfig.pin_lux_sda = PIN_LUX_SDA;
                if (currentConfig.pin_lux_scl < 0)
                    currentConfig.pin_lux_scl = PIN_LUX_SCL;
                break;
            default:
                break;
            }

            // Server-side forbidden-pin / duplicate check (Part 5.4/5.5)
            // applies here too, in case a restored default somehow
            // collides with another sensor's currently-assigned pin.
            PinCheckEntry proposed[] = {
                {currentConfig.pin_tds, "TDS"},
                {currentConfig.pin_dht, "DHT22"},
                {currentConfig.pin_ph, "pH"},
                {currentConfig.pin_ds18b20, "DS18B20 (Water Temp)"},
                {currentConfig.pin_wl, "Water Level Signal"},
                {currentConfig.pin_lux_sda, "BH1750 SDA"},
                {currentConfig.pin_lux_scl, "BH1750 SCL"},
                {currentConfig.pin_wl_power, "Water Level Power"},
            };
            String problem = validatePinSet(proposed, 8);
            if (problem.length() > 0)
            {
                webLog(0, LOG_ERR, "save_sensor_enabled rejected: " + problem);
                sendCmdAck(client, cmd, false, problem);
                return;
            }
        }

        if (!state_save())
        {
            webLog(0, LOG_ERR, "save_sensor_enabled: state_save() failed — settings may not be fully persisted.");
            sendCmdAck(client, cmd, false, "Failed to save. Device storage may be full or corrupted.");
            return;
        }
        broadcastConfig();
        webLog(0, LOG_WARN, "Sensor '" + sensor + "' " + String(enabled ? "enabled" : "disabled") + ". Reboot required to apply.");
        sendCmdAck(client, cmd, true);
    }
    else if (cmd == "save_intervals")
    {
        // Same "omit-to-leave-unchanged" pattern as save_pins. Bounds
        // (2000-60000ms) are enforced client-side in the Timing card;
        // clamp server-side too so a hand-crafted message can't set an
        // absurd interval.
        uint32_t r = doc["int_read"] | currentConfig.interval_read_ms;
        uint32_t w = doc["int_ws"] | currentConfig.interval_ws_ms;
        uint32_t v = doc["int_vit"] | currentConfig.interval_vitals_ms;
        uint32_t f = doc["int_fb"] | currentConfig.interval_fb_ms;

        auto clamp = [](uint32_t x) -> uint32_t
        { return x < 2000 ? 2000 : (x > 60000 ? 60000 : x); };

        currentConfig.interval_read_ms = clamp(r);
        currentConfig.interval_ws_ms = clamp(w);
        currentConfig.interval_vitals_ms = clamp(v);
        currentConfig.interval_fb_ms = clamp(f);
        if (!state_save())
        {
            webLog(0, LOG_ERR, "save_intervals: state_save() failed — settings may not be fully persisted.");
            sendCmdAck(client, cmd, false, "Failed to save. Device storage may be full or corrupted.");
            return;
        }
        broadcastConfig();
        webLog(0, LOG_INFO, "Timing intervals updated.");
        sendCmdAck(client, cmd, true);
    }
    else if (cmd == "reset_sensor_pin")
    {
        String sensor = doc["sensor"] | "";
        bool matched = true;

        // Part 5.1 fix: restoring the pin(s) alone used to leave
        // sensor_enabled[id] permanently false once a sensor had
        // auto-disabled (see autoDisable() in task_sensor.cpp) — nothing
        // else in the firmware ever set it back to true. Flip the flag
        // back on here, in the same place the pin gets restored, so
        // "Reset" from the Web UI actually undoes an auto-disable instead
        // of just moving the pin back while the sensor stays off forever.
        if (sensor == "tds")
        {
            currentConfig.pin_tds = PIN_TDS;
            currentConfig.sensor_enabled[S_TDS] = true;
        }
        else if (sensor == "dht")
        {
            currentConfig.pin_dht = PIN_DHT;
            currentConfig.sensor_enabled[S_DHT] = true;
        }
        else if (sensor == "ph")
        {
            currentConfig.pin_ph = PIN_PH;
            currentConfig.sensor_enabled[S_PH] = true;
        }
        else if (sensor == "wt")
        {
            currentConfig.pin_ds18b20 = PIN_DS18B20;
            currentConfig.sensor_enabled[S_WTEMP] = true;
        }
        else if (sensor == "wl")
        {
            currentConfig.pin_wl = PIN_WL;
            currentConfig.pin_wl_power = PIN_WL_PWR;
            currentConfig.sensor_enabled[S_WL] = true;
        }
        else if (sensor == "light")
        {
            currentConfig.pin_lux_sda = PIN_LUX_SDA;
            currentConfig.pin_lux_scl = PIN_LUX_SCL;
            currentConfig.sensor_enabled[S_LIGHT] = true;
        }
        else
            matched = false;

        if (matched)
        {
            if (!state_save())
            {
                webLog(0, LOG_ERR, "reset_sensor_pin: state_save() failed — reset not persisted. Aborting reboot.");
                sendCmdAck(client, cmd, false, "Failed to save. Device storage may be full or corrupted.");
                return;
            }
            webLog(0, LOG_WARN, "Pin(s) for '" + sensor + "' reset to compiled default and re-enabled. Rebooting...");
            delay(1000);
            ESP.restart();
        }
        else
        {
            webLog(0, LOG_ERR, "reset_sensor_pin: unknown sensor id '" + sensor + "'");
            sendCmdAck(client, cmd, false, "Unknown sensor id '" + sensor + "'");
        }
    }
    else if (cmd == "factory_reset")
    {
        webLog(0, LOG_WARN, "Factory reset initiated. Wiping NVS...");
        state_factory_reset(); // This handles clear, end, and restart natively.
    }
    else if (cmd == "reboot")
    {
        webLog(0, LOG_WARN, "Manual reboot requested...");
        delay(1000);
        ESP.restart();
    }
    else if (cmd == "request_vitals")
    {
        broadcastVitals();
    }
    else
    {
        webLog(0, LOG_WARN, "Unknown command: '" + cmd + "'");
        sendCmdAck(client, cmd, false, "Unknown command.");
    }
}
