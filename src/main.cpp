#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>

#define WIFI_SSID  "NO WIFI FOR YOU!!!"
#define WIFI_PASS  "Nestle2010Nestle"
#define WS_HOST    "192.168.0.122"
#define WS_PORT    8765
#define WS_PATH    "/"

// Globals

constexpr size_t MAX_CONFIG_JSON_LENGTH = 8192;
constexpr size_t MAX_RSSI_VALUES = 16;

struct SnapshotState {
    char type[16];
    char service_state[16];
    int service_pid;
    char service_active_since[64];
    char config_mtime[32];
    float config_mtime_ago_days;
    char current_log_file[96];
    char config_json[MAX_CONFIG_JSON_LENGTH];
    size_t config_json_length;
    size_t config_section_count;
    bool config_was_truncated;
    char radioid_csv_file[128];
    bool radioid_csv_exists;
    char radioid_csv_mtime[32];
    int radioid_csv_age_hours;
    bool radioid_csv_is_stale;
    int radioid_entries;
    bool radioid_lookup_loaded;
    char radioid_status[24];
    char radioid_last_refresh_attempt[32];
    char radioid_last_refresh_success[32];
    char radioid_last_refresh_error[160];
    char station_callsign[24];
    int station_match_count;
    char station_id[16];
    char station_name[32];
    char station_surname[32];
    char station_city[48];
    char station_state[48];
    char station_country[32];
    char station_country_code[4];
    bool valid;
} g_snapshot;

struct LiveState {
    char type[16];
    int event_id;
    char timestamp[32];
    char mode[16];
    char last_event[32];
    char direction[16];
    bool slot_valid;
    int slot;
    char source[24];
    int source_match_count;
    char source_id[16];
    char source_callsign[24];
    char source_name[32];
    char source_surname[32];
    char source_city[48];
    char source_state[48];
    char source_country[32];
    char source_country_code[4];
    char destination[32];
    char talker_alias[64];
    bool duration_valid;
    float duration_sec;
    bool packet_loss_valid;
    float packet_loss_percent;
    bool ber_valid;
    float ber_percent;
    int rssi_values_dbm[MAX_RSSI_VALUES];
    size_t rssi_count;
    char raw_line[196];
    bool valid;
} g_live;

WebSocketsClient ws;

// Helpers

void clearSnapshotState() {
    memset(&g_snapshot, 0, sizeof(g_snapshot));
    strlcpy(g_snapshot.type, "snapshot", sizeof(g_snapshot.type));
}

void clearLiveState() {
    memset(&g_live, 0, sizeof(g_live));
    strlcpy(g_live.type, "live", sizeof(g_live.type));
}

void copyJsonString(JsonVariantConst value, char* destination, size_t destinationSize) {
    if (destinationSize == 0) {
        return;
    }

    if (value.isNull()) {
        destination[0] = '\0';
        return;
    }

    String text = value.as<String>();
    strlcpy(destination, text.c_str(), destinationSize);
}

void printDivider(const char* title) {
    Serial.println();
    Serial.println("========================================");
    Serial.printf("%s\n", title);
    Serial.println("========================================");
}

void printConfigSections(JsonVariantConst configVariant) {
    Serial.printf("Config Sections: %u\n", static_cast<unsigned>(g_snapshot.config_section_count));
    if (g_snapshot.config_was_truncated) {
        Serial.println("WARNING: Config payload exceeded local storage and was truncated.");
    }

    JsonObjectConst configObject = configVariant.as<JsonObjectConst>();
    if (configObject.isNull()) {
        Serial.println("Config payload unavailable.");
        return;
    }

    for (JsonPairConst sectionPair : configObject) {
        Serial.println();
        Serial.printf("[%s]\n", sectionPair.key().c_str());

        JsonObjectConst sectionObject = sectionPair.value().as<JsonObjectConst>();
        if (sectionObject.isNull()) {
            Serial.println("  <non-object section>");
            continue;
        }

        for (JsonPairConst entryPair : sectionObject) {
            String valueText = entryPair.value().as<String>();
            Serial.printf("  %-18s = %s\n", entryPair.key().c_str(), valueText.c_str());
        }
    }
}

void printRssiValues() {
    Serial.print("RSSI dBm          : ");
    if (g_live.rssi_count == 0) {
        Serial.println("n/a");
        return;
    }

    for (size_t index = 0; index < g_live.rssi_count; ++index) {
        if (index > 0) {
            Serial.print(", ");
        }
        Serial.print(g_live.rssi_values_dbm[index]);
    }
    Serial.println();
}

bool liveEventHasDetailPayload() {
    return g_live.slot_valid ||
           g_live.source[0] != '\0' ||
           g_live.destination[0] != '\0' ||
           g_live.talker_alias[0] != '\0' ||
           g_live.duration_valid ||
           g_live.packet_loss_valid ||
           g_live.ber_valid ||
           g_live.rssi_count > 0;
}

bool isEmptyTalkerAliasEvent() {
    return strcmp(g_live.last_event, "talker_alias") == 0 &&
           g_live.talker_alias[0] == '\0' &&
           g_live.source[0] == '\0' &&
           g_live.destination[0] == '\0' &&
           !g_live.duration_valid &&
           !g_live.packet_loss_valid &&
           !g_live.ber_valid &&
           g_live.rssi_count == 0;
}

void printSnapshot(JsonVariantConst configVariant) {
    printDivider("PI-STAR SNAPSHOT");
    Serial.printf("Type              : %s\n", g_snapshot.type);
    Serial.printf("Service State     : %s\n", g_snapshot.service_state);
    Serial.printf("Service PID       : %d\n", g_snapshot.service_pid);
    Serial.printf("Active Since      : %s\n", g_snapshot.service_active_since);
    Serial.printf("Config MTime      : %s\n", g_snapshot.config_mtime);
    Serial.printf("Config Age Days   : %.2f\n", g_snapshot.config_mtime_ago_days);
    Serial.printf("Current Log File  : %s\n", g_snapshot.current_log_file);
    Serial.printf("RadioID CSV File  : %s\n", g_snapshot.radioid_csv_file);
    Serial.printf("RadioID CSV Exists: %s\n", g_snapshot.radioid_csv_exists ? "yes" : "no");
    Serial.printf("RadioID CSV MTime : %s\n", g_snapshot.radioid_csv_mtime);
    Serial.printf("RadioID CSV Age   : %d hours\n", g_snapshot.radioid_csv_age_hours);
    Serial.printf("RadioID CSV Stale : %s\n", g_snapshot.radioid_csv_is_stale ? "yes" : "no");
    Serial.printf("RadioID Entries   : %d\n", g_snapshot.radioid_entries);
    Serial.printf("RadioID Loaded    : %s\n", g_snapshot.radioid_lookup_loaded ? "yes" : "no");
    Serial.printf("RadioID Status    : %s\n", g_snapshot.radioid_status);
    Serial.printf("Last Refresh Try  : %s\n", g_snapshot.radioid_last_refresh_attempt);
    Serial.printf("Last Refresh OK   : %s\n", g_snapshot.radioid_last_refresh_success);
    Serial.printf("Last Refresh Error: %s\n", g_snapshot.radioid_last_refresh_error);
    Serial.printf("Station Callsign  : %s\n", g_snapshot.station_callsign);
    Serial.printf("Station Matches   : %d\n", g_snapshot.station_match_count);
    Serial.printf("Station ID        : %s\n", g_snapshot.station_id);
    Serial.printf("Station Name      : %s\n", g_snapshot.station_name);
    Serial.printf("Station Surname   : %s\n", g_snapshot.station_surname);
    Serial.printf("Station City      : %s\n", g_snapshot.station_city);
    Serial.printf("Station State     : %s\n", g_snapshot.station_state);
    Serial.printf("Station Country   : %s\n", g_snapshot.station_country);
    Serial.printf("Station ISO Code  : %s\n", g_snapshot.station_country_code);
    printConfigSections(configVariant);
    Serial.println("========================================");
}

void printLive() {
    if (isEmptyTalkerAliasEvent()) {
        Serial.printf("[LIVE] empty talker_alias #%d | slot=%d | raw=%s\n",
            g_live.event_id,
            g_live.slot_valid ? g_live.slot : -1,
            g_live.raw_line[0] != '\0' ? g_live.raw_line : "n/a");
        return;
    }

    if (!liveEventHasDetailPayload()) {
        Serial.printf("[LIVE] sparse event #%d | %s | mode=%s | raw=%s\n",
            g_live.event_id,
            g_live.last_event[0] != '\0' ? g_live.last_event : "unknown",
            g_live.mode[0] != '\0' ? g_live.mode : "unknown",
            g_live.raw_line[0] != '\0' ? g_live.raw_line : "n/a");
        return;
    }

    printDivider("PI-STAR LIVE EVENT");
    Serial.printf("Type              : %s\n", g_live.type);
    Serial.printf("Event ID          : %d\n", g_live.event_id);
    Serial.printf("Timestamp         : %s\n", g_live.timestamp);
    Serial.printf("Mode              : %s\n", g_live.mode);
    Serial.printf("Last Event        : %s\n", g_live.last_event);
    Serial.printf("Direction         : %s\n", g_live.direction);
    Serial.printf("Slot              : %s", g_live.slot_valid ? "" : "n/a");
    if (g_live.slot_valid) {
        Serial.printf("%d\n", g_live.slot);
    } else {
        Serial.println();
    }
    Serial.printf("Source            : %s\n", g_live.source);
    Serial.printf("Source Matches    : %d\n", g_live.source_match_count);
    Serial.printf("Source ID         : %s\n", g_live.source_id);
    Serial.printf("Source Callsign   : %s\n", g_live.source_callsign);
    Serial.printf("Source Name       : %s\n", g_live.source_name);
    Serial.printf("Source Surname    : %s\n", g_live.source_surname);
    Serial.printf("Source City       : %s\n", g_live.source_city);
    Serial.printf("Source State      : %s\n", g_live.source_state);
    Serial.printf("Source Country    : %s\n", g_live.source_country);
    Serial.printf("Source ISO Code   : %s\n", g_live.source_country_code);
    Serial.printf("Destination       : %s\n", g_live.destination);
    Serial.printf("Talker Alias      : %s\n", g_live.talker_alias);
    Serial.printf("Duration Sec      : %s", g_live.duration_valid ? "" : "n/a");
    if (g_live.duration_valid) {
        Serial.printf("%.2f\n", g_live.duration_sec);
    } else {
        Serial.println();
    }
    Serial.printf("Packet Loss Pct   : %s", g_live.packet_loss_valid ? "" : "n/a");
    if (g_live.packet_loss_valid) {
        Serial.printf("%.2f\n", g_live.packet_loss_percent);
    } else {
        Serial.println();
    }
    Serial.printf("BER Percent       : %s", g_live.ber_valid ? "" : "n/a");
    if (g_live.ber_valid) {
        Serial.printf("%.2f\n", g_live.ber_percent);
    } else {
        Serial.println();
    }
    printRssiValues();
    Serial.printf("Raw Line          : %s\n", g_live.raw_line);
    Serial.println("========================================");
}

void storeConfigJson(JsonVariantConst configVariant) {
    JsonObjectConst configObject = configVariant.as<JsonObjectConst>();
    if (configObject.isNull()) {
        g_snapshot.config_json[0] = '\0';
        g_snapshot.config_json_length = 0;
        g_snapshot.config_section_count = 0;
        g_snapshot.config_was_truncated = false;
        return;
    }

    g_snapshot.config_section_count = configObject.size();
    const size_t requiredLength = measureJson(configVariant);
    g_snapshot.config_was_truncated = requiredLength >= sizeof(g_snapshot.config_json);
    g_snapshot.config_json_length = serializeJson(configVariant, g_snapshot.config_json, sizeof(g_snapshot.config_json));
}

// Parsers

void parseSnapshot(JsonDocument& doc) {
    clearSnapshotState();

    copyJsonString(doc["type"], g_snapshot.type, sizeof(g_snapshot.type));
    copyJsonString(doc["service"]["state"], g_snapshot.service_state, sizeof(g_snapshot.service_state));
    g_snapshot.service_pid = doc["service"]["main_pid"] | 0;
    copyJsonString(doc["service"]["active_since"], g_snapshot.service_active_since, sizeof(g_snapshot.service_active_since));
    copyJsonString(doc["config_mtime"], g_snapshot.config_mtime, sizeof(g_snapshot.config_mtime));
    g_snapshot.config_mtime_ago_days = doc["config_mtime_ago_days"] | 0.0f;
    copyJsonString(doc["current_log_file"], g_snapshot.current_log_file, sizeof(g_snapshot.current_log_file));
    storeConfigJson(doc["config"]);
    copyJsonString(doc["radioid_csv_file"], g_snapshot.radioid_csv_file, sizeof(g_snapshot.radioid_csv_file));
    g_snapshot.radioid_csv_exists = doc["radioid_csv_exists"] | false;
    copyJsonString(doc["radioid_csv_mtime"], g_snapshot.radioid_csv_mtime, sizeof(g_snapshot.radioid_csv_mtime));
    g_snapshot.radioid_csv_age_hours = doc["radioid_csv_age_hours"] | 0;
    g_snapshot.radioid_csv_is_stale = doc["radioid_csv_is_stale"] | true;
    g_snapshot.radioid_entries = doc["radioid_entries"] | 0;
    g_snapshot.radioid_lookup_loaded = doc["radioid_lookup_loaded"] | false;
    copyJsonString(doc["radioid_status"], g_snapshot.radioid_status, sizeof(g_snapshot.radioid_status));
    copyJsonString(doc["radioid_last_refresh_attempt"], g_snapshot.radioid_last_refresh_attempt, sizeof(g_snapshot.radioid_last_refresh_attempt));
    copyJsonString(doc["radioid_last_refresh_success"], g_snapshot.radioid_last_refresh_success, sizeof(g_snapshot.radioid_last_refresh_success));
    copyJsonString(doc["radioid_last_refresh_error"], g_snapshot.radioid_last_refresh_error, sizeof(g_snapshot.radioid_last_refresh_error));
    copyJsonString(doc["station_callsign"], g_snapshot.station_callsign, sizeof(g_snapshot.station_callsign));
    g_snapshot.station_match_count = doc["station_match_count"] | 0;
    copyJsonString(doc["station_id"], g_snapshot.station_id, sizeof(g_snapshot.station_id));
    copyJsonString(doc["station_name"], g_snapshot.station_name, sizeof(g_snapshot.station_name));
    copyJsonString(doc["station_surname"], g_snapshot.station_surname, sizeof(g_snapshot.station_surname));
    copyJsonString(doc["station_city"], g_snapshot.station_city, sizeof(g_snapshot.station_city));
    copyJsonString(doc["station_state"], g_snapshot.station_state, sizeof(g_snapshot.station_state));
    copyJsonString(doc["station_country"], g_snapshot.station_country, sizeof(g_snapshot.station_country));
    copyJsonString(doc["station_country_code"], g_snapshot.station_country_code, sizeof(g_snapshot.station_country_code));
    g_snapshot.valid = true;

    printSnapshot(doc["config"]);
}

void parseLive(JsonDocument& doc) {
    clearLiveState();

    copyJsonString(doc["type"], g_live.type, sizeof(g_live.type));
    g_live.event_id = doc["event_id"] | 0;
    copyJsonString(doc["timestamp"], g_live.timestamp, sizeof(g_live.timestamp));
    copyJsonString(doc["mode"], g_live.mode, sizeof(g_live.mode));
    copyJsonString(doc["last_event"], g_live.last_event, sizeof(g_live.last_event));
    copyJsonString(doc["direction"], g_live.direction, sizeof(g_live.direction));
    g_live.slot_valid = !doc["slot"].isNull();
    g_live.slot = g_live.slot_valid ? (doc["slot"] | 0) : 0;
    copyJsonString(doc["source"], g_live.source, sizeof(g_live.source));
    g_live.source_match_count = doc["source_match_count"] | 0;
    copyJsonString(doc["source_id"], g_live.source_id, sizeof(g_live.source_id));
    copyJsonString(doc["source_callsign"], g_live.source_callsign, sizeof(g_live.source_callsign));
    copyJsonString(doc["source_name"], g_live.source_name, sizeof(g_live.source_name));
    copyJsonString(doc["source_surname"], g_live.source_surname, sizeof(g_live.source_surname));
    copyJsonString(doc["source_city"], g_live.source_city, sizeof(g_live.source_city));
    copyJsonString(doc["source_state"], g_live.source_state, sizeof(g_live.source_state));
    copyJsonString(doc["source_country"], g_live.source_country, sizeof(g_live.source_country));
    copyJsonString(doc["source_country_code"], g_live.source_country_code, sizeof(g_live.source_country_code));
    copyJsonString(doc["destination"], g_live.destination, sizeof(g_live.destination));
    copyJsonString(doc["talker_alias"], g_live.talker_alias, sizeof(g_live.talker_alias));
    g_live.duration_valid = !doc["duration_sec"].isNull();
    g_live.duration_sec = g_live.duration_valid ? (doc["duration_sec"] | 0.0f) : 0.0f;
    g_live.packet_loss_valid = !doc["packet_loss_percent"].isNull();
    g_live.packet_loss_percent = g_live.packet_loss_valid ? (doc["packet_loss_percent"] | 0.0f) : 0.0f;
    g_live.ber_valid = !doc["ber_percent"].isNull();
    g_live.ber_percent = g_live.ber_valid ? (doc["ber_percent"] | 0.0f) : 0.0f;
    copyJsonString(doc["raw_line"], g_live.raw_line, sizeof(g_live.raw_line));

    JsonArrayConst rssiArray = doc["rssi_values_dbm"].as<JsonArrayConst>();
    g_live.rssi_count = 0;
    if (!rssiArray.isNull()) {
        for (JsonVariantConst rssiValue : rssiArray) {
            if (g_live.rssi_count >= MAX_RSSI_VALUES) {
                break;
            }
            g_live.rssi_values_dbm[g_live.rssi_count++] = rssiValue | 0;
        }
    }

    g_live.valid = true;
    printLive();
}

// Connection helpers

void primeArp() {
    WiFiClient tcp;
    while (!tcp.connect(WS_HOST, WS_PORT)) { delay(500); }
    tcp.stop();
    delay(100);
}

// WebSocket

void onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_DISCONNECTED:
            Serial.println("[WS] Disconnected");
            primeArp();
            break;
        case WStype_CONNECTED:
            Serial.println("[WS] Connected");
            break;
        case WStype_TEXT: {
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, payload, length);
            if (err) {
                Serial.printf("[WS] JSON error: %s\n", err.c_str());
                break;
            }
            const char* t = doc["type"] | "";
            if      (strcmp(t, "snapshot") == 0) parseSnapshot(doc);
            else if (strcmp(t, "live")     == 0) parseLive(doc);
            break;
        }
        default:
            break;
    }
}

// Setup / loop

void setup() {
    Serial.begin(115200);
    clearSnapshotState();
    clearLiveState();

    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.printf("\nConnected! IP: %s\n", WiFi.localIP().toString().c_str());

    primeArp();

    ws.begin(WS_HOST, WS_PORT, WS_PATH);
    ws.onEvent(onWsEvent);
    ws.setReconnectInterval(5000);
    ws.enableHeartbeat(15000, 3000, 2);  // ping every 15s, pong timeout 3s, disconnect after 2 misses
}

void loop() {
    ws.loop();
}
