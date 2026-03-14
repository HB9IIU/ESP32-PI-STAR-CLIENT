#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <time.h>
#include <sys/time.h>

#define WIFI_SSID "NO WIFI FOR YOU!!!"
#define WIFI_PASS "Nestle2010Nestle"
#define WS_HOST "192.168.0.122"
#define WS_PORT 8765
#define WS_PATH "/"

// Globals

constexpr size_t MAX_CONFIG_JSON_LENGTH = 8192;
constexpr size_t MAX_RSSI_VALUES = 16;
constexpr size_t MAX_HEARD_RECENT_ITEMS = 10;
constexpr uint32_t WS_RECOVERY_INTERVAL_MS = 5000;
constexpr uint32_t WIFI_RECOVERY_INTERVAL_MS = 10000;

struct SnapshotState
{
    char type[16];
    bool server_time_valid;
    int64_t server_time_unix;
    char server_time_iso[40];
    int server_utc_offset_sec;
    char service_state[16];
    int service_pid;
    char service_active_since[64];
    char config_mtime[32];
    int config_mtime_ago_hours;
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

struct LiveState
{
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

struct HeardRecentItem
{
    char callsign[24];
    char name[32];
    char country_code[4];
    char last_seen[32];
    char last_tg[32];
};

struct HeardSummaryState
{
    char type[20];
    int unique_callsigns_total;
    char last_heard_callsign[24];
    char last_heard_at[32];
    HeardRecentItem recent[MAX_HEARD_RECENT_ITEMS];
    size_t recent_count;
    bool valid;
} g_heardSummary;

WebSocketsClient ws;
bool g_clockInitialized = false;
int g_clockUtcOffsetSec = 0;
char g_lastClockText[6] = "";
char g_lastRxCallsignText[24] = "";
unsigned long g_nextWsRecoveryMs = 0;
unsigned long g_nextWiFiRecoveryMs = 0;

void drawClockDemo(const char *timeText);
void updateHotspotCallsignDisplay();
void updateRxCallsignDisplay();
void configureWebSocketClient();
void onWsEvent(WStype_t type, uint8_t *payload, size_t length);

// Helpers

void clearSnapshotState()
{
    memset(&g_snapshot, 0, sizeof(g_snapshot));
    strlcpy(g_snapshot.type, "snapshot", sizeof(g_snapshot.type));
}

void clearLiveState()
{
    memset(&g_live, 0, sizeof(g_live));
    strlcpy(g_live.type, "live", sizeof(g_live.type));
}

void clearHeardSummaryState()
{
    memset(&g_heardSummary, 0, sizeof(g_heardSummary));
    strlcpy(g_heardSummary.type, "heard_summary", sizeof(g_heardSummary.type));
}

void copyJsonString(JsonVariantConst value, char *destination, size_t destinationSize)
{
    if (destinationSize == 0)
    {
        return;
    }

    if (value.isNull())
    {
        destination[0] = '\0';
        return;
    }

    String text = value.as<String>();
    strlcpy(destination, text.c_str(), destinationSize);
}

void printDivider(const char *title)
{
    Serial.println();
    Serial.println("========================================");
    Serial.printf("%s\n", title);
    Serial.println("========================================");
}

void printConfigSections(JsonVariantConst configVariant)
{
    Serial.printf("Config Sections: %u\n", static_cast<unsigned>(g_snapshot.config_section_count));
    if (g_snapshot.config_was_truncated)
    {
        Serial.println("WARNING: Config payload exceeded local storage and was truncated.");
    }

    JsonObjectConst configObject = configVariant.as<JsonObjectConst>();
    if (configObject.isNull())
    {
        Serial.println("Config payload unavailable.");
        return;
    }

    for (JsonPairConst sectionPair : configObject)
    {
        Serial.println();
        Serial.printf("[%s]\n", sectionPair.key().c_str());

        JsonObjectConst sectionObject = sectionPair.value().as<JsonObjectConst>();
        if (sectionObject.isNull())
        {
            Serial.println("  <non-object section>");
            continue;
        }

        for (JsonPairConst entryPair : sectionObject)
        {
            String valueText = entryPair.value().as<String>();
            Serial.printf("  %-18s = %s\n", entryPair.key().c_str(), valueText.c_str());
        }
    }
}

void printRssiValues()
{
    Serial.print("RSSI dBm          : ");
    if (g_live.rssi_count == 0)
    {
        Serial.println("n/a");
        return;
    }

    for (size_t index = 0; index < g_live.rssi_count; ++index)
    {
        if (index > 0)
        {
            Serial.print(", ");
        }
        Serial.print(g_live.rssi_values_dbm[index]);
    }
    Serial.println();
}

bool liveEventHasDetailPayload()
{
    return g_live.slot_valid ||
           g_live.source[0] != '\0' ||
           g_live.destination[0] != '\0' ||
           g_live.talker_alias[0] != '\0' ||
           g_live.duration_valid ||
           g_live.packet_loss_valid ||
           g_live.ber_valid ||
           g_live.rssi_count > 0;
}

bool isEmptyTalkerAliasEvent()
{
    return strcmp(g_live.last_event, "talker_alias") == 0 &&
           g_live.talker_alias[0] == '\0' &&
           g_live.source[0] == '\0' &&
           g_live.destination[0] == '\0' &&
           !g_live.duration_valid &&
           !g_live.packet_loss_valid &&
           !g_live.ber_valid &&
           g_live.rssi_count == 0;
}

void printSnapshot(JsonVariantConst configVariant)
{
    printDivider("PI-STAR SNAPSHOT");
    Serial.printf("Type              : %s\n", g_snapshot.type);
    Serial.printf("Server Time Unix  : %s", g_snapshot.server_time_valid ? "" : "n/a");
    if (g_snapshot.server_time_valid)
    {
        Serial.printf("%lld\n", static_cast<long long>(g_snapshot.server_time_unix));
    }
    else
    {
        Serial.println();
    }
    Serial.printf("Server Time ISO   : %s\n", g_snapshot.server_time_iso);
    Serial.printf("UTC Offset Sec    : %d\n", g_snapshot.server_utc_offset_sec);
    Serial.printf("Service State     : %s\n", g_snapshot.service_state);
    Serial.printf("Service PID       : %d\n", g_snapshot.service_pid);
    Serial.printf("Active Since      : %s\n", g_snapshot.service_active_since);
    Serial.printf("Config MTime      : %s\n", g_snapshot.config_mtime);
    Serial.printf("Config Age Hours  : %d\n", g_snapshot.config_mtime_ago_hours);
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

void printHeardSummary()
{
    printDivider("HEARD SUMMARY");
    Serial.printf("Type              : %s\n", g_heardSummary.type);
    Serial.printf("Unique Callsigns  : %d\n", g_heardSummary.unique_callsigns_total);
    Serial.printf("Last Heard        : %s\n", g_heardSummary.last_heard_callsign);
    Serial.printf("Last Heard At     : %s\n", g_heardSummary.last_heard_at);
    Serial.printf("Recent Entries    : %u\n", static_cast<unsigned>(g_heardSummary.recent_count));

    for (size_t index = 0; index < g_heardSummary.recent_count; ++index)
    {
        const HeardRecentItem &item = g_heardSummary.recent[index];
        Serial.printf("  %u. %s | %s | %s | %s\n",
                      static_cast<unsigned>(index + 1),
                      item.callsign,
                      item.name,
                      item.country_code,
                      item.last_tg);
        Serial.printf("     Last Seen: %s\n", item.last_seen);
    }

    Serial.println("========================================");
}

void printLive()
{
    if (isEmptyTalkerAliasEvent())
    {
        Serial.printf("[LIVE] empty talker_alias #%d | slot=%d | raw=%s\n",
                      g_live.event_id,
                      g_live.slot_valid ? g_live.slot : -1,
                      g_live.raw_line[0] != '\0' ? g_live.raw_line : "n/a");
        return;
    }

    if (!liveEventHasDetailPayload())
    {
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
    if (g_live.slot_valid)
    {
        Serial.printf("%d\n", g_live.slot);
    }
    else
    {
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
    if (g_live.duration_valid)
    {
        Serial.printf("%.2f\n", g_live.duration_sec);
    }
    else
    {
        Serial.println();
    }
    Serial.printf("Packet Loss Pct   : %s", g_live.packet_loss_valid ? "" : "n/a");
    if (g_live.packet_loss_valid)
    {
        Serial.printf("%.2f\n", g_live.packet_loss_percent);
    }
    else
    {
        Serial.println();
    }
    Serial.printf("BER Percent       : %s", g_live.ber_valid ? "" : "n/a");
    if (g_live.ber_valid)
    {
        Serial.printf("%.2f\n", g_live.ber_percent);
    }
    else
    {
        Serial.println();
    }
    printRssiValues();
    Serial.printf("Raw Line          : %s\n", g_live.raw_line);
    Serial.println("========================================");
}

void storeConfigJson(JsonVariantConst configVariant)
{
    JsonObjectConst configObject = configVariant.as<JsonObjectConst>();
    if (configObject.isNull())
    {
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

void initializeClockFromSnapshot()
{
    if (g_clockInitialized || !g_snapshot.server_time_valid)
    {
        return;
    }

    timeval timeValue = {};
    timeValue.tv_sec = static_cast<time_t>(g_snapshot.server_time_unix);

    if (settimeofday(&timeValue, nullptr) != 0)
    {
        Serial.println("[TIME] Failed to initialize clock from snapshot");
        return;
    }

    g_clockUtcOffsetSec = g_snapshot.server_utc_offset_sec;
    g_clockInitialized = true;
    g_lastClockText[0] = '\0';
    Serial.printf("[TIME] Clock initialized from snapshot: %lld (%s)\n",
                  static_cast<long long>(g_snapshot.server_time_unix),
                  g_snapshot.server_time_iso);
}

void buildClockText(char *destination, size_t destinationSize)
{
    if (destinationSize == 0)
    {
        return;
    }

    if (!g_clockInitialized)
    {
        strlcpy(destination, "--:--", destinationSize);
        return;
    }

    const time_t nowUtc = time(nullptr);
    if (nowUtc <= 0)
    {
        strlcpy(destination, "--:--", destinationSize);
        return;
    }

    const time_t nowLocal = nowUtc + g_clockUtcOffsetSec;
    struct tm timeInfo = {};
    gmtime_r(&nowLocal, &timeInfo);

    if (strftime(destination, destinationSize, "%H:%M", &timeInfo) == 0)
    {
        strlcpy(destination, "--:--", destinationSize);
    }
}

void updateClockDisplay(bool force = false)
{
    char clockText[6];
    buildClockText(clockText, sizeof(clockText));

    if (!force && strcmp(clockText, g_lastClockText) == 0)
    {
        return;
    }

    drawClockDemo(clockText);
    strlcpy(g_lastClockText, clockText, sizeof(g_lastClockText));
}

// Parsers

void parseSnapshot(JsonDocument &doc)
{
    clearSnapshotState();

    copyJsonString(doc["type"], g_snapshot.type, sizeof(g_snapshot.type));
    g_snapshot.server_time_valid = !doc["server_time_unix"].isNull();
    g_snapshot.server_time_unix = g_snapshot.server_time_valid ? doc["server_time_unix"].as<int64_t>() : 0;
    copyJsonString(doc["server_time_iso"], g_snapshot.server_time_iso, sizeof(g_snapshot.server_time_iso));
    g_snapshot.server_utc_offset_sec = doc["server_utc_offset_sec"] | 0;
    copyJsonString(doc["service"]["state"], g_snapshot.service_state, sizeof(g_snapshot.service_state));
    g_snapshot.service_pid = doc["service"]["main_pid"] | 0;
    copyJsonString(doc["service"]["active_since"], g_snapshot.service_active_since, sizeof(g_snapshot.service_active_since));
    copyJsonString(doc["config_mtime"], g_snapshot.config_mtime, sizeof(g_snapshot.config_mtime));
    g_snapshot.config_mtime_ago_hours = doc["config_mtime_ago_hours"] | 0;
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

    initializeClockFromSnapshot();
    updateHotspotCallsignDisplay();
    updateClockDisplay(true);

    printSnapshot(doc["config"]);
}

void parseLive(JsonDocument &doc)
{
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
    if (!rssiArray.isNull())
    {
        for (JsonVariantConst rssiValue : rssiArray)
        {
            if (g_live.rssi_count >= MAX_RSSI_VALUES)
            {
                break;
            }
            g_live.rssi_values_dbm[g_live.rssi_count++] = rssiValue | 0;
        }
    }

    g_live.valid = true;
    updateRxCallsignDisplay();
    printLive();
}

void parseHeardSummary(JsonDocument &doc)
{
    clearHeardSummaryState();

    copyJsonString(doc["type"], g_heardSummary.type, sizeof(g_heardSummary.type));
    g_heardSummary.unique_callsigns_total = doc["unique_callsigns_total"] | 0;
    copyJsonString(doc["last_heard_callsign"], g_heardSummary.last_heard_callsign, sizeof(g_heardSummary.last_heard_callsign));
    copyJsonString(doc["last_heard_at"], g_heardSummary.last_heard_at, sizeof(g_heardSummary.last_heard_at));

    JsonArrayConst recentArray = doc["recent"].as<JsonArrayConst>();
    g_heardSummary.recent_count = 0;
    if (!recentArray.isNull())
    {
        for (JsonObjectConst recentItem : recentArray)
        {
            if (g_heardSummary.recent_count >= MAX_HEARD_RECENT_ITEMS)
            {
                break;
            }

            HeardRecentItem &item = g_heardSummary.recent[g_heardSummary.recent_count++];
            copyJsonString(recentItem["callsign"], item.callsign, sizeof(item.callsign));
            copyJsonString(recentItem["name"], item.name, sizeof(item.name));
            copyJsonString(recentItem["country_code"], item.country_code, sizeof(item.country_code));
            copyJsonString(recentItem["last_seen"], item.last_seen, sizeof(item.last_seen));
            copyJsonString(recentItem["last_tg"], item.last_tg, sizeof(item.last_tg));
        }
    }

    g_heardSummary.valid = true;
    printHeardSummary();
}

// Connection helpers

bool primeArp(uint8_t maxAttempts = 3)
{
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("[WS] ARP prime skipped: WiFi not connected");
        return false;
    }

    WiFiClient tcp;
    for (uint8_t attempt = 1; attempt <= maxAttempts; ++attempt)
    {
        if (tcp.connect(WS_HOST, WS_PORT))
        {
            tcp.stop();
            delay(100);
            return true;
        }
        delay(250);
    }

    Serial.println("[WS] ARP prime skipped: host not reachable yet");
    return false;
}

void configureWebSocketClient()
{
    ws.begin(WS_HOST, WS_PORT, WS_PATH);
    ws.onEvent(onWsEvent);
    ws.setReconnectInterval(5000);
    ws.enableHeartbeat(15000, 3000, 2);
}

void ensureConnectivity()
{
    const unsigned long now = millis();

    if (WiFi.status() != WL_CONNECTED)
    {
        if (now < g_nextWiFiRecoveryMs)
        {
            return;
        }

        g_nextWiFiRecoveryMs = now + WIFI_RECOVERY_INTERVAL_MS;
        Serial.println("[WiFi] Connection lost, attempting reconnect");
        WiFi.disconnect();
        WiFi.reconnect();
        return;
    }

    if (ws.isConnected())
    {
        return;
    }

    if (now < g_nextWsRecoveryMs)
    {
        return;
    }

    g_nextWsRecoveryMs = now + WS_RECOVERY_INTERVAL_MS;

    Serial.println("[WS] Recovery attempt");
    if (!primeArp(2))
    {
        Serial.println("[WS] Recovery deferred: host still unreachable");
        return;
    }

    ws.disconnect();
    configureWebSocketClient();
}

// WebSocket

void onWsEvent(WStype_t type, uint8_t *payload, size_t length)
{
    switch (type)
    {
    case WStype_DISCONNECTED:
        Serial.println("[WS] Disconnected");
        g_nextWsRecoveryMs = 0;
        break;
    case WStype_CONNECTED:
        Serial.println("[WS] Connected");
        g_nextWsRecoveryMs = millis() + WS_RECOVERY_INTERVAL_MS;
        break;
    case WStype_TEXT:
    {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, payload, length);
        if (err)
        {
            Serial.printf("[WS] JSON error: %s\n", err.c_str());
            break;
        }
        const char *t = doc["type"] | "";
        if (strcmp(t, "snapshot") == 0)
            parseSnapshot(doc);
        else if (strcmp(t, "live") == 0)
            parseLive(doc);
        else if (strcmp(t, "heard_summary") == 0)
            parseHeardSummary(doc);
        break;
    }
    default:
        break;
    }
}

#include <SPI.h>
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>
#include <XPT2046_Touchscreen.h>

#include "RobotoCondensedBold24px7b.h"
#include "RobotoCondensedBold36px7b.h"
#include "RobotoCondensedBold12px7b.h"
#include "RobotoMonoRegular10px7b.h"
#include "RobotoMonoRegular12px7b.h"
#include "RobotoMonoRegular20px7b.h"
#include "RobotoCondensedRegular16px7b.h"
#include "UbuntuMonoBold18px7b.h"

TFT_eSPI tft = TFT_eSPI();

SPIClass touchscreenSPI = SPIClass(VSPI);
XPT2046_Touchscreen touchscreen(XPT2046_CS, XPT2046_IRQ);

#define FONT_SIZE 2

constexpr uint32_t COUNTRY_MAP_DEMO_INTERVAL_MS = 4000;
constexpr int LEFT_COLUMN_SEPARATOR_WIDTH = 222;

const char *kDemoCountryCodes[] = {"CH", "DE", "FR", "GB", "JP", "US"};
constexpr size_t kDemoCountryCodeCount = sizeof(kDemoCountryCodes) / sizeof(kDemoCountryCodes[0]);
const char *kSmallFlagRowDemoCountryCodes[] = {"AU", "BE", "BR", "CA", "CH", "DE", "DK", "ES", "FR", "GB", "IT", "JP", "NL", "SE", "US"};
constexpr size_t kSmallFlagRowDemoCountryCodeCount = sizeof(kSmallFlagRowDemoCountryCodes) / sizeof(kSmallFlagRowDemoCountryCodes[0]);

size_t g_demoCountryIndex = 0;
unsigned long g_nextCountryMapDemoMs = 0;

bool tftJpegOutput(int16_t x, int16_t y, uint16_t width, uint16_t height, uint16_t *bitmap)
{
    if (y >= tft.height() || x >= tft.width())
    {
        return false;
    }

    if (x + width > tft.width())
    {
        width = tft.width() - x;
    }

    if (y + height > tft.height())
    {
        height = tft.height() - y;
    }

    tft.pushImage(x, y, width, height, bitmap);
    return true;
}

bool looksLikeCountryCode(const char *countryCode)
{
    return countryCode != nullptr &&
           strlen(countryCode) == 2 &&
           isAlpha(countryCode[0]) &&
           isAlpha(countryCode[1]);
}

void buildCountryAssetPath(const char *countryCode, const char *sizeFolder, char *path, size_t pathSize)
{
    char normalizedCode[3] = {'\0', '\0', '\0'};
    normalizedCode[0] = static_cast<char>(toupper(countryCode[0]));
    normalizedCode[1] = static_cast<char>(toupper(countryCode[1]));

    snprintf(path, pathSize, "/flags/%s/%s.jpg", sizeFolder, normalizedCode);
}

bool displayCountryMapFromFolder(const char *countryCode, const char *sizeFolder, int x, int y)
{
    if (!looksLikeCountryCode(countryCode))
    {
        Serial.printf("[TFT] Invalid country code: %s\n", countryCode != nullptr ? countryCode : "<null>");
        return false;
    }

    char assetPath[32];
    buildCountryAssetPath(countryCode, sizeFolder, assetPath, sizeof(assetPath));

    if (!SPIFFS.exists(assetPath))
    {
        Serial.printf("[TFT] Missing asset: %s\n", assetPath);
        tft.drawRect(x, y, 48, 32, TFT_RED);
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.drawString(countryCode, x + 4, y + 8, FONT_SIZE);
        return false;
    }

    Serial.printf("[TFT] Drawing %s at (%d,%d)\n", assetPath, x, y);
    return TJpgDec.drawFsJpg(x, y, assetPath, SPIFFS);
}

bool displayContryMapLarge(const char *countryCode, int x, int y)
{
    return displayCountryMapFromFolder(countryCode, "large", x, y);
}

bool displayContryMapSmall(const char *countryCode, int x, int y)
{
    return displayCountryMapFromFolder(countryCode, "small", x, y);
}

const char *currentDemoCountryCode()
{
    if (looksLikeCountryCode(g_live.source_country_code))
    {
        return g_live.source_country_code;
    }

    if (looksLikeCountryCode(g_snapshot.station_country_code))
    {
        return g_snapshot.station_country_code;
    }

    return kDemoCountryCodes[g_demoCountryIndex];
}

void drawTopCallsignFontDemo(const char *callsign)
{
    const char *text = (callsign != nullptr && callsign[0] != '\0') ? callsign : "CONNECTING";

    tft.fillRect(0, 0, tft.width(), 32, TFT_NAVY);
    tft.drawFastHLine(0, 31, tft.width(), TFT_CYAN);

    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_NAVY);
    tft.setFreeFont(&UbuntuMonoBold18px7b);
    tft.drawString(text, tft.width() / 2, 5);

    tft.setFreeFont(nullptr);
    tft.setTextDatum(TL_DATUM);
}

void updateHotspotCallsignDisplay()
{
    const char *callsign = (g_snapshot.valid && g_snapshot.station_callsign[0] != '\0')
                               ? g_snapshot.station_callsign
                               : "CONNECTING";

    drawTopCallsignFontDemo(callsign);
}

bool liveCallsignIsActive()
{
    return strcmp(g_live.last_event, "rf_voice_header") == 0 ||
           strcmp(g_live.last_event, "network_voice_header") == 0 ||
           strcmp(g_live.last_event, "talker_alias") == 0;
}

void drawRxCallsignText(const char *callsign, uint16_t textColor)
{
    const char *text = (callsign != nullptr && callsign[0] != '\0') ? callsign : "Idle";
    const int textX = 72;
    const int textY = 52;

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(textColor, TFT_BLACK);
    tft.setFreeFont(&RobotoCondensedBold36px7b);
    tft.drawString(text, textX, textY);
    tft.setFreeFont(nullptr);
}

void updateRxCallsignDisplay()
{
    const char *nextText = (g_live.valid && liveCallsignIsActive() && g_live.source_callsign[0] != '\0')
                               ? g_live.source_callsign
                               : "Idle";

    if (g_lastRxCallsignText[0] != '\0')
    {
        drawRxCallsignText(g_lastRxCallsignText, TFT_BLACK);
    }

    drawRxCallsignText(nextText, TFT_WHITE);
    strlcpy(g_lastRxCallsignText, nextText, sizeof(g_lastRxCallsignText));
}

void drawFlagAndCallsignDemo(const char *countryCode, const char *callsign)
{
    const char *code = looksLikeCountryCode(countryCode) ? countryCode : "US";
    const char *text = (callsign != nullptr && callsign[0] != '\0') ? callsign : "Idle";

    const int y = 50;
    const int height = 58;
    const int flagX = 10;
    const int flagY = 56;
    const int textX = 72;
    const int textY = 52;
    const int separatorY = 103;

    tft.fillRect(0, y, tft.width(), height, TFT_BLACK);
    displayContryMapLarge(code, flagX, flagY);

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setFreeFont(&RobotoCondensedBold36px7b);
    tft.drawString(text, textX, textY);
    tft.drawFastHLine(0, separatorY, tft.width(), TFT_CYAN);

    tft.setFreeFont(nullptr);
}

void drawNameFontDemo(const char *name)
{
    const char *text = (name != nullptr && name[0] != '\0') ? name : "PAUL";
    const uint16_t gold = tft.color565(245, 200, 60);

    const int x = 10;
    const int y = 109;
    const int width = tft.width() - x;
    const int height = 34;

    tft.fillRect(x, y, width, height, TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(gold, TFT_BLACK);
    tft.setFreeFont(&RobotoCondensedBold24px7b);
    tft.drawString(text, x, y);

    tft.setFreeFont(nullptr);
}

void drawLocationFontDemo(const char *location)
{
    const char *text = (location != nullptr && location[0] != '\0') ? location : "KOKOMO, UNITED STATES";
    const uint16_t textColor = tft.color565(232, 232, 232);

    const int x = 10;
    const int dividerY = 144;
    const int y = 150;
    const int width = tft.width() - x;
    const int height = 24;

    tft.fillRect(x, y, width, height, TFT_BLACK);
    tft.drawFastHLine(0, dividerY, tft.width(), TFT_CYAN);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(textColor, TFT_BLACK);
    tft.setFreeFont(&RobotoCondensedRegular16px7b);
    tft.drawString(text, x, y);

    tft.setFreeFont(nullptr);
}

void drawTalkgroupRowDemo(const char *label)
{
    const char *text = (label != nullptr && label[0] != '\0') ? label : "TS2 WORLD WIDE";
    const uint16_t textColor = tft.color565(215, 235, 245);

    const int x = 10;
    const int dividerY = 171;
    const int y = 175;
    const int width = tft.width() - x;
    const int height = 28;
    const int bottomSeparatorY = 197;

    tft.fillRect(x, y, width, height, TFT_BLACK);
    tft.drawFastHLine(0, dividerY, tft.width(), TFT_CYAN);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(textColor, TFT_BLACK);
    tft.setFreeFont(&RobotoCondensedRegular16px7b);
    tft.drawString(text, x, y);
    tft.drawFastHLine(0, bottomSeparatorY, tft.width(), TFT_CYAN);

    tft.setFreeFont(nullptr);
}

void drawActivityStatusBannerDemo(const char *statusText)
{
    const char *text = (statusText != nullptr && statusText[0] != '\0') ? statusText : "RX";
    const bool isTx = strcmp(text, "TX") == 0;
    const uint16_t fillColor = isTx ? tft.color565(210, 45, 45) : tft.color565(35, 145, 60);

    const int x = 10;
    const int y = 36;
    const int bodyWidth = 116;
    const int arrowWidth = 12;
    const int height = 14;
    const int midY = y + (height / 2);

    tft.fillRect(x - 1, y - 1, bodyWidth + arrowWidth + 2, height + 2, TFT_BLACK);
    tft.fillRect(x, y, bodyWidth, height, fillColor);
    tft.fillTriangle(x + bodyWidth, y,
                     x + bodyWidth + arrowWidth, midY,
                     x + bodyWidth, y + height,
                     fillColor);

    tft.setTextDatum(CC_DATUM);
    tft.setTextColor(TFT_WHITE, fillColor);
    tft.setFreeFont(&RobotoCondensedBold12px7b);
    tft.drawString(text, x + (bodyWidth / 2), y + (height / 2));

    tft.setFreeFont(nullptr);
    tft.setTextDatum(TL_DATUM);
}

void drawClockDemo(const char *timeText)
{
    const char *text = (timeText != nullptr && timeText[0] != '\0') ? timeText : "--:--";
    const uint16_t textColor = TFT_WHITE;
    const uint16_t backgroundColor = TFT_NAVY;

    const int rightMargin = 8;
    const int y = 7;
    const int width = 86;
    const int height = 14;
    const int x = tft.width() - rightMargin - width;

    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(textColor, backgroundColor);
    tft.setFreeFont(&RobotoMonoRegular12px7b);
    tft.drawString(text, tft.width() - rightMargin, y);

    tft.setFreeFont(nullptr);
    tft.setTextDatum(TL_DATUM);
}

void drawRssiMetricDemo(const char *valueText)
{
    const char *text = (valueText != nullptr && valueText[0] != '\0') ? valueText : "-73";
    const uint16_t labelColor = tft.color565(165, 205, 220);
    const uint16_t valueColor = TFT_WHITE;
    const int x = 246;
    const int x1 = 316;
    const int y = 39;

    tft.fillRect(x, y, 88, 14, TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(labelColor, TFT_BLACK);
    tft.setFreeFont(&RobotoCondensedBold12px7b);
    tft.drawString("RSSI", x, y);
    tft.setTextColor(valueColor, TFT_BLACK);
    tft.setFreeFont(&RobotoMonoRegular12px7b);
    tft.setTextDatum(TR_DATUM);
    tft.drawString(text, x1, y);
    tft.setFreeFont(nullptr);
    tft.setTextDatum(TL_DATUM);
}

void drawBerMetricDemo(const char *valueText)
{
    const char *text = (valueText != nullptr && valueText[0] != '\0') ? valueText : "xx";
    const uint16_t labelColor = tft.color565(165, 205, 220);
    const uint16_t valueColor = TFT_WHITE;
    const int x = 246;
    const int x1 = 316;
    const int y = 61;

    tft.fillRect(x, y, 88, 14, TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(labelColor, TFT_BLACK);
    tft.setFreeFont(&RobotoCondensedBold12px7b);
    tft.drawString("BER", x, y);
    tft.setTextColor(valueColor, TFT_BLACK);
    tft.setFreeFont(&RobotoMonoRegular12px7b);
    tft.setTextDatum(TR_DATUM);
    tft.drawString(text, x1, y);
    tft.setFreeFont(nullptr);
    tft.setTextDatum(TL_DATUM);
}

void drawLossMetricDemo(const char *valueText)
{
    const char *text = (valueText != nullptr && valueText[0] != '\0') ? valueText : "yy";
    const uint16_t labelColor = tft.color565(165, 205, 220);
    const uint16_t valueColor = TFT_WHITE;
    const int x = 246;
    const int x1 = 316;
    const int y = 83;

    tft.fillRect(x, y, 88, 14, TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(labelColor, TFT_BLACK);
    tft.setFreeFont(&RobotoCondensedBold12px7b);
    tft.drawString("LOSS", x, y);
    tft.setTextColor(valueColor, TFT_BLACK);
    tft.setFreeFont(&RobotoMonoRegular12px7b);
    tft.setTextDatum(TR_DATUM);
    tft.drawString(text, x1, y);
    tft.setFreeFont(nullptr);
    tft.setTextDatum(TL_DATUM);
}

void drawDurationMetricDemo(const char *valueText)
{
    const char *text = (valueText != nullptr && valueText[0] != '\0') ? valueText : "0:42";
    const uint16_t valueColor = TFT_WHITE;
    const int x1 = 316;
    const int y = 108;
    const int iconX = x1 - 100;

    tft.fillRect(200, y, 120, 28, TFT_BLACK);
    if (SPIFFS.exists("/timer.jpg"))
    {
        TJpgDec.drawFsJpg(iconX, y + 1, "/timer.jpg", SPIFFS);
    }
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(valueColor, TFT_BLACK);
    tft.setFreeFont(&RobotoMonoRegular20px7b);
    tft.drawString(text, x1, y);
    tft.setFreeFont(nullptr);
    tft.setTextDatum(TL_DATUM);
}

void drawRecentHeardFlagsDemo()
{
    constexpr size_t kVisibleFlagCount = 8;
    constexpr int kFlagWidth = 24;
    constexpr int kFlagHeight = 17;

    const int startX = 17;
    const int stepX = 37;
    const int y = 206;
    const int rowWidth = (static_cast<int>(kVisibleFlagCount) - 1) * stepX + kFlagWidth;

    size_t shuffledIndices[kSmallFlagRowDemoCountryCodeCount];
    for (size_t index = 0; index < kSmallFlagRowDemoCountryCodeCount; ++index)
    {
        shuffledIndices[index] = index;
    }

    for (size_t index = kSmallFlagRowDemoCountryCodeCount - 1; index > 0; --index)
    {
        const size_t swapIndex = static_cast<size_t>(random(static_cast<long>(index + 1)));
        const size_t temp = shuffledIndices[index];
        shuffledIndices[index] = shuffledIndices[swapIndex];
        shuffledIndices[swapIndex] = temp;
    }

    tft.fillRect(startX, y, rowWidth, kFlagHeight, TFT_BLACK);

    for (size_t index = 0; index < kVisibleFlagCount; ++index)
    {
        const int x = startX + static_cast<int>(index) * stepX;
        const char *countryCode = kSmallFlagRowDemoCountryCodes[shuffledIndices[index]];
        displayContryMapSmall(countryCode, x, y);
    }
}

void drawFooterStatusDemo(const char *statusText)
{
    const char *text = (statusText != nullptr && statusText[0] != '\0') ? statusText : "439.500 MHz | TS2 | 4 heard today";
    const uint16_t textColor = tft.color565(205, 215, 225);

    const int y = 228;
    const int height = 12;

    tft.fillRect(0, y, tft.width(), height, TFT_BLACK);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(textColor, TFT_BLACK);
    tft.setFreeFont(&RobotoMonoRegular10px7b);
    tft.drawString(text, tft.width() / 2, y);

    tft.setFreeFont(nullptr);
    tft.setTextDatum(TL_DATUM);
}

void renderCountryMapDemo(const char *countryCode)
{
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("Country map test", 8, 8, FONT_SIZE);
    tft.drawString(countryCode, 230, 8, FONT_SIZE);

    displayContryMapLarge(countryCode, 8, 40);
    displayContryMapSmall(countryCode, 220, 40);
}

void updateCountryMapDemo()
{
    const unsigned long now = millis();
    if (now < g_nextCountryMapDemoMs)
    {
        return;
    }

    const bool usingLiveCode = looksLikeCountryCode(g_live.source_country_code) ||
                               looksLikeCountryCode(g_snapshot.station_country_code);
    const char *countryCode = currentDemoCountryCode();
    renderCountryMapDemo(countryCode);

    if (!usingLiveCode)
    {
        g_demoCountryIndex = (g_demoCountryIndex + 1) % kDemoCountryCodeCount;
    }

    g_nextCountryMapDemoMs = now + COUNTRY_MAP_DEMO_INTERVAL_MS;
}

// Print Touchscreen info about X, Y and Pressure (Z) on the Serial Monitor
void printTouchToSerial(int touchX, int touchY, int touchZ)
{
    Serial.print("X = ");
    Serial.print(touchX);
    Serial.print(" | Y = ");
    Serial.print(touchY);
    Serial.print(" | Pressure = ");
    Serial.print(touchZ);
    Serial.println();
}

// Print Touchscreen info about X, Y and Pressure (Z) on the TFT Display
void printTouchToDisplay(int touchX, int touchY, int touchZ)
{
    // Clear TFT screen
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_BLACK, TFT_WHITE);

    int centerX = tft.width() / 2;
    int textY = 80;

    String tempText = "X = " + String(touchX);
    tft.drawCentreString(tempText, centerX, textY, FONT_SIZE);

    textY += 20;
    tempText = "Y = " + String(touchY);
    tft.drawCentreString(tempText, centerX, textY, FONT_SIZE);

    textY += 20;
    tempText = "Pressure = " + String(touchZ);
    tft.drawCentreString(tempText, centerX, textY, FONT_SIZE);
}

// Setup / loop

void setup()
{
    Serial.begin(115200);

    // Start the SPI for the touchscreen and init the touchscreen
    touchscreenSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
    touchscreen.begin(touchscreenSPI);
    // Set the Touchscreen rotation in landscape mode
    // Note: in some displays, the touchscreen might be upside down, so you might need to set the rotation to 3: touchscreen.setRotation(3);
    touchscreen.setRotation(1);

    // Start the tft display
    tft.init();
    // Set the TFT display rotation in landscape mode
    tft.setRotation(1);
    tft.setSwapBytes(true);

    if (!SPIFFS.begin(true))
    {
        Serial.println("[SPIFFS] Mount failed");
    }
    else
    {
        Serial.println("[SPIFFS] Mounted");
    }

    TJpgDec.setJpgScale(1);
    TJpgDec.setCallback(tftJpegOutput);

    // Clear the screen before writing to it
    tft.fillScreen(TFT_BLACK);
    randomSeed(micros());
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    updateHotspotCallsignDisplay();
    updateClockDisplay(true);
    drawActivityStatusBannerDemo("RX");
    drawFlagAndCallsignDemo("US", "Idle");
    drawNameFontDemo("PAUL");
    drawDurationMetricDemo("0:42");
    drawRssiMetricDemo("-73");
    drawBerMetricDemo("xx");
    drawLossMetricDemo("yy");
    drawLocationFontDemo("KOKOMO, UNITED STATES");
    drawTalkgroupRowDemo("TS2 WORLD WIDE");
    drawRecentHeardFlagsDemo();
    drawFooterStatusDemo("439.500 MHz | TS2 | 4 heard today");

    clearSnapshotState();
    clearLiveState();
    clearHeardSummaryState();

    WiFi.begin(WIFI_SSID, WIFI_PASS);
    WiFi.setAutoReconnect(true);
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");
    }
    Serial.printf("\nConnected! IP: %s\n", WiFi.localIP().toString().c_str());

    primeArp();

    configureWebSocketClient();
}
// Touchscreen coordinates: (x, y) and pressure (z)
int x, y, z;
void loop()
{
    ensureConnectivity();
    ws.loop();
    updateClockDisplay();
    // updateCountryMapDemo();

    // Checks if Touchscreen was touched, and prints X, Y and Pressure (Z) info on the TFT display and Serial Monitor
    if (touchscreen.tirqTouched() && touchscreen.touched())
    {
        // Get Touchscreen points
        TS_Point p = touchscreen.getPoint();
        // Calibrate Touchscreen points with map function to the correct width and height
        x = map(p.x, 200, 3700, 1, tft.width());
        y = map(p.y, 240, 3800, 1, tft.height());
        z = p.z;

        printTouchToSerial(x, y, z);

        delay(100);
    }
}
