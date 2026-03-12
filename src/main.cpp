#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>

#define WIFI_SSID  "NO WIFI FOR YOU!!!"
#define WIFI_PASS  "Nestle2010Nestle"
#define WS_HOST    "192.168.0.122"
#define WS_PORT    8765
#define WS_PATH    "/"

// ── Globals ───────────────────────────────────────────────────────────────────

struct SnapshotState {
    // service
    char  service_state[16];
    int   service_pid;
    char  service_active_since[64];
    // General
    char  callsign[16];
    char  id[16];
    char  duplex[4];
    char  display[16];
    // Info
    char  location[64];
    char  description[64];
    char  url[80];
    char  rx_freq[16];
    char  tx_freq[16];
    char  power[8];
    char  latitude[16];
    char  longitude[16];
    // DMR
    char  dmr_color_code[4];
    char  dmr_network_type[16];   // Gateway / Direct
    // Modem
    char  modem_port[32];
    char  modem_uart_speed[8];
    // country
    char  country_code[4];
    char  country_name[32];
    // log
    char  current_log_file[80];
    bool  valid;
} g_snapshot;

struct LiveState {
    int   event_id;
    char  timestamp[32];
    char  mode[16];
    char  last_event[32];
    char  direction[16];
    int   slot;                    // -1 = null
    char  source[32];
    char  source_name[32];
    char  source_city[32];
    char  source_country_code[4];
    char  source_country_name[32];
    char  destination[32];
    char  talker_alias[64];
    float duration_sec;            // -1 = null
    float packet_loss_pct;         // -1 = null
    float ber_pct;                 // -1 = null
    int   rssi[8];
    int   rssi_count;
    bool  valid;
} g_live;

WebSocketsClient ws;

// ── Parsers ───────────────────────────────────────────────────────────────────

void parseSnapshot(JsonDocument& doc) {
    strlcpy(g_snapshot.service_state,        doc["service"]["state"]           | "", sizeof(g_snapshot.service_state));
    g_snapshot.service_pid =                 doc["service"]["main_pid"]        | 0;
    strlcpy(g_snapshot.service_active_since, doc["service"]["active_since"]    | "", sizeof(g_snapshot.service_active_since));
    strlcpy(g_snapshot.callsign,          doc["config"]["General"]["Callsign"]        | "", sizeof(g_snapshot.callsign));
    strlcpy(g_snapshot.id,                doc["config"]["General"]["Id"]              | "", sizeof(g_snapshot.id));
    strlcpy(g_snapshot.duplex,            doc["config"]["General"]["Duplex"]          | "", sizeof(g_snapshot.duplex));
    strlcpy(g_snapshot.display,           doc["config"]["General"]["Display"]         | "", sizeof(g_snapshot.display));
    strlcpy(g_snapshot.location,          doc["config"]["Info"]["Location"]           | "", sizeof(g_snapshot.location));
    strlcpy(g_snapshot.description,       doc["config"]["Info"]["Description"]        | "", sizeof(g_snapshot.description));
    strlcpy(g_snapshot.url,               doc["config"]["Info"]["URL"]                | "", sizeof(g_snapshot.url));
    strlcpy(g_snapshot.rx_freq,           doc["config"]["Info"]["RXFrequency"]        | "", sizeof(g_snapshot.rx_freq));
    strlcpy(g_snapshot.tx_freq,           doc["config"]["Info"]["TXFrequency"]        | "", sizeof(g_snapshot.tx_freq));
    strlcpy(g_snapshot.power,             doc["config"]["Info"]["Power"]              | "", sizeof(g_snapshot.power));
    strlcpy(g_snapshot.latitude,          doc["config"]["Info"]["Latitude"]           | "", sizeof(g_snapshot.latitude));
    strlcpy(g_snapshot.longitude,         doc["config"]["Info"]["Longitude"]          | "", sizeof(g_snapshot.longitude));
    strlcpy(g_snapshot.dmr_color_code,    doc["config"]["DMR"]["ColorCode"]           | "", sizeof(g_snapshot.dmr_color_code));
    strlcpy(g_snapshot.dmr_network_type,  doc["config"]["DMR Network"]["Type"]        | "", sizeof(g_snapshot.dmr_network_type));
    strlcpy(g_snapshot.modem_port,        doc["config"]["Modem"]["UARTPort"]          | "", sizeof(g_snapshot.modem_port));
    strlcpy(g_snapshot.modem_uart_speed,  doc["config"]["Modem"]["UARTSpeed"]         | "", sizeof(g_snapshot.modem_uart_speed));
    strlcpy(g_snapshot.country_code,       doc["country_code"]                         | "", sizeof(g_snapshot.country_code));
    strlcpy(g_snapshot.country_name,       doc["country_name"]                         | "", sizeof(g_snapshot.country_name));
    strlcpy(g_snapshot.current_log_file,   doc["current_log_file"]                     | "", sizeof(g_snapshot.current_log_file));
    g_snapshot.valid = true;

    Serial.println("=== PI-STAR SNAPSHOT ===");
    Serial.printf("Callsign  : %s  (DMR ID: %s)\n",  g_snapshot.callsign, g_snapshot.id);
    Serial.printf("Country   : %s (%s)\n",            g_snapshot.country_name, g_snapshot.country_code);
    Serial.printf("Location  : %s\n",                 g_snapshot.location);
    Serial.printf("Desc      : %s\n",                 g_snapshot.description);
    Serial.printf("URL       : %s\n",                 g_snapshot.url);
    Serial.printf("Coords    : %s, %s\n",             g_snapshot.latitude, g_snapshot.longitude);
    Serial.printf("RX / TX   : %s / %s Hz\n",         g_snapshot.rx_freq, g_snapshot.tx_freq);
    Serial.printf("Power     : %s W\n",               g_snapshot.power);
    Serial.printf("DMR CC    : %s  Network: %s\n",    g_snapshot.dmr_color_code, g_snapshot.dmr_network_type);
    Serial.printf("Duplex    : %s  Display: %s\n",    g_snapshot.duplex, g_snapshot.display);
    Serial.printf("Modem     : %s @ %s baud\n",       g_snapshot.modem_port, g_snapshot.modem_uart_speed);
    Serial.printf("Service   : %s  PID=%d\n",         g_snapshot.service_state, g_snapshot.service_pid);
    Serial.printf("Since     : %s\n",                 g_snapshot.service_active_since);
    Serial.printf("Log       : %s\n",                 g_snapshot.current_log_file);
    Serial.printf("DB entries: %d\n",                 doc["db_entries"] | 0);
    Serial.println("========================");
}

void parseLive(JsonDocument& doc) {
    g_live.event_id = doc["event_id"] | 0;
    strlcpy(g_live.timestamp,    doc["timestamp"]    | "", sizeof(g_live.timestamp));
    strlcpy(g_live.mode,         doc["mode"]         | "", sizeof(g_live.mode));
    strlcpy(g_live.last_event,   doc["last_event"]   | "", sizeof(g_live.last_event));
    strlcpy(g_live.direction,    doc["direction"]    | "", sizeof(g_live.direction));
    strlcpy(g_live.source,              doc["source"]              | "", sizeof(g_live.source));
    strlcpy(g_live.source_name,         doc["source_name"]         | "", sizeof(g_live.source_name));
    strlcpy(g_live.source_city,         doc["source_city"]         | "", sizeof(g_live.source_city));
    strlcpy(g_live.source_country_code, doc["source_country_code"] | "", sizeof(g_live.source_country_code));
    strlcpy(g_live.source_country_name, doc["source_country_name"] | "", sizeof(g_live.source_country_name));
    strlcpy(g_live.destination,         doc["destination"]         | "", sizeof(g_live.destination));
    strlcpy(g_live.talker_alias, doc["talker_alias"] | "", sizeof(g_live.talker_alias));

    g_live.slot        = doc["slot"].isNull()               ? -1    : (int)doc["slot"];
    g_live.duration_sec= doc["duration_sec"].isNull()        ? -1.0f : (float)doc["duration_sec"];
    g_live.packet_loss_pct = doc["packet_loss_percent"].isNull() ? -1.0f : (float)doc["packet_loss_percent"];
    g_live.ber_pct     = doc["ber_percent"].isNull()         ? -1.0f : (float)doc["ber_percent"];

    g_live.rssi_count = 0;
    for (JsonVariant v : doc["rssi_values_dbm"].as<JsonArray>()) {
        if (g_live.rssi_count < 8) g_live.rssi[g_live.rssi_count++] = v.as<int>();
    }
    g_live.valid = true;

    Serial.printf("[LIVE] #%d %-24s | mode=%-8s | %s (%s, %s %s) -> %s slot=%d\n",
        g_live.event_id, g_live.last_event, g_live.mode,
        g_live.source, g_live.source_name, g_live.source_city,
        g_live.source_country_code, g_live.destination, g_live.slot);
}

// ── Helpers ───────────────────────────────────────────────────────────────────

void primeArp() {
    WiFiClient tcp;
    while (!tcp.connect(WS_HOST, WS_PORT)) { delay(500); }
    tcp.stop();
    delay(100);
}

// ── WebSocket ─────────────────────────────────────────────────────────────────

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

// ── Setup / Loop ──────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);

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
