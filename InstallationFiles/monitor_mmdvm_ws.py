#!/usr/bin/env python3

import asyncio
import configparser
import copy
import csv
import glob
import json
import os
import re
import shutil
import subprocess
import time
import urllib.request
from datetime import datetime, timezone

import websockets

# ---------------------------------------------------------------------
# APP PATHS
# ---------------------------------------------------------------------

APP_DIR = os.path.dirname(os.path.abspath(__file__))

# ---------------------------------------------------------------------
# CONFIG
# ---------------------------------------------------------------------

MMDVM_CONFIG_FILE = "/etc/mmdvmhost"
MMDVM_LOG_GLOB = "/var/log/pi-star/MMDVM-*.log"

WS_BIND_HOST = "0.0.0.0"
WS_BIND_PORT = 8765

LOG_POLL_INTERVAL_SECONDS = 0.2
RECHECK_INTERVAL_SECONDS = 2.0

RADIOID_CSV_URL = "https://www.radioid.net/static/user.csv"
RADIOID_LOCAL_CSV = os.path.join(APP_DIR, "radioid_user.csv")
RADIOID_REFRESH_INTERVAL_SECONDS = 24 * 3600
RADIOID_REFRESH_CHECK_INTERVAL_SECONDS = 300
RADIOID_DOWNLOAD_TIMEOUT_SECONDS = 60

# ---------------------------------------------------------------------
# GLOBAL STATE
# ---------------------------------------------------------------------

CLIENTS = set()

SNAPSHOT_STATE = {
    "type": "snapshot",
    "service": {
        "state": "",
        "main_pid": 0,
        "active_since": ""
    },
    "config_mtime": "",
    "config_mtime_ago_days": 0.0,
    "current_log_file": "",
    "config": {},
    "radioid_csv_file": RADIOID_LOCAL_CSV,
    "radioid_csv_mtime": 0.0,
    "radioid_entries": 0,
    "station_callsign": "",
    "station_match_count": 0,
    "station_id": "",
    "station_name": "",
    "station_surname": "",
    "station_city": "",
    "station_state": "",
    "station_country": "",
    "station_country_code": ""
}

LIVE_STATE = {
    "type": "live",
    "event_id": 0,
    "timestamp": "",
    "mode": "Unknown",
    "last_event": "",
    "direction": "",
    "slot": None,
    "source": "",
    "source_match_count": 0,
    "source_id": "",
    "source_callsign": "",
    "source_name": "",
    "source_surname": "",
    "source_city": "",
    "source_state": "",
    "source_country": "",
    "source_country_code": "",
    "destination": "",
    "talker_alias": "",
    "duration_sec": None,
    "packet_loss_percent": None,
    "ber_percent": None,
    "rssi_values_dbm": [],
    "raw_line": ""
}

# ---------------------------------------------------------------------
# REGEX
# ---------------------------------------------------------------------

REGEX_MODE_SET = re.compile(
    r"^M:\s+(?P<timestamp>\d{4}-\d{2}-\d{2}\s+\d{2}:\d{2}:\d{2}\.\d{3})\s+Mode set to (?P<mode>.+)$"
)

REGEX_RF_HEADER = re.compile(
    r"^M:\s+(?P<timestamp>\d{4}-\d{2}-\d{2}\s+\d{2}:\d{2}:\d{2}\.\d{3})\s+DMR Slot (?P<slot>\d), received RF voice header from (?P<source>.+?) to (?P<destination>.+)$"
)

REGEX_NETWORK_HEADER = re.compile(
    r"^M:\s+(?P<timestamp>\d{4}-\d{2}-\d{2}\s+\d{2}:\d{2}:\d{2}\.\d{3})\s+DMR Slot (?P<slot>\d), received network voice header from (?P<source>.+?) to (?P<destination>.+)$"
)

REGEX_TALKER_ALIAS = re.compile(
    r'^M:\s+(?P<timestamp>\d{4}-\d{2}-\d{2}\s+\d{2}:\d{2}:\d{2}\.\d{3})\s+DMR Slot (?P<slot>\d), Talker Alias "(?P<alias>.*)"$'
)

REGEX_RF_END_LOST = re.compile(
    r"^M:\s+(?P<timestamp>\d{4}-\d{2}-\d{2}\s+\d{2}:\d{2}:\d{2}\.\d{3})\s+DMR Slot (?P<slot>\d), RF voice transmission lost from (?P<source>.+?) to (?P<destination>.+?), (?P<duration>[0-9.]+) seconds, BER: (?P<ber>[0-9.]+)%, RSSI: (?P<rssi>.+)$"
)

REGEX_RF_END = re.compile(
    r"^M:\s+(?P<timestamp>\d{4}-\d{2}-\d{2}\s+\d{2}:\d{2}:\d{2}\.\d{3})\s+DMR Slot (?P<slot>\d), received RF end of voice transmission from (?P<source>.+?) to (?P<destination>.+?), (?P<duration>[0-9.]+) seconds, BER: (?P<ber>[0-9.]+)%, RSSI: (?P<rssi>.+)$"
)

REGEX_NETWORK_END = re.compile(
    r"^M:\s+(?P<timestamp>\d{4}-\d{2}-\d{2}\s+\d{2}:\d{2}:\d{2}\.\d{3})\s+DMR Slot (?P<slot>\d), received network end of voice transmission from (?P<source>.+?) to (?P<destination>.+?), (?P<duration>[0-9.]+) seconds, (?P<packet_loss>[0-9.]+)% packet loss, BER: (?P<ber>[0-9.]+)%$"
)

# ---------------------------------------------------------------------
# SYSTEM HELPERS
# ---------------------------------------------------------------------

def run_command(command_list):
    try:
        result = subprocess.run(
            command_list,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            universal_newlines=True,
            check=False
        )
        return result.stdout.strip()
    except Exception:
        return ""


def get_service_state():
    return run_command(["systemctl", "is-active", "mmdvmhost"])


def get_service_main_pid():
    value = run_command(["systemctl", "show", "-p", "MainPID", "--value", "mmdvmhost"])
    try:
        return int(value)
    except Exception:
        return 0


def get_service_active_since():
    return run_command(["systemctl", "show", "-p", "ActiveEnterTimestamp", "--value", "mmdvmhost"])


def get_config_file_mtime():
    try:
        return os.path.getmtime(MMDVM_CONFIG_FILE)
    except OSError:
        return 0.0


def find_latest_log_file():
    file_list = glob.glob(MMDVM_LOG_GLOB)
    if not file_list:
        return ""
    file_list.sort(key=lambda path: os.path.getmtime(path), reverse=True)
    return file_list[0]


def open_log_file_at_end(log_file_path):
    handle = open(log_file_path, "r", encoding="utf-8", errors="replace")
    handle.seek(0, os.SEEK_END)
    return handle


def get_watch_state_tuple():
    return (
        get_config_file_mtime(),
        get_service_main_pid(),
        find_latest_log_file()
    )


def get_file_mtime(path):
    try:
        return os.path.getmtime(path)
    except OSError:
        return 0.0


def format_timestamp_utc(timestamp_value):
    if not timestamp_value:
        return ""
    return datetime.fromtimestamp(timestamp_value, tz=timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")


def calculate_age_days(timestamp_value):
    if not timestamp_value:
        return 0.0
    age_seconds = max(0.0, time.time() - timestamp_value)
    return round(age_seconds / 86400.0, 2)

# ---------------------------------------------------------------------
# COUNTRY LOOKUP
# ---------------------------------------------------------------------

COUNTRY_NAME_TO_ISO = {
    "Albania": "AL", "Andorra": "AD", "Argentina": "AR",
    "Armenia": "AM", "Australia": "AU", "Austria": "AT",
    "Azerbaijan": "AZ", "Belarus": "BY", "Belgium": "BE",
    "Bosnia": "BA", "Brazil": "BR", "Bulgaria": "BG",
    "Canada": "CA", "Chile": "CL", "China": "CN",
    "Colombia": "CO", "Croatia": "HR", "Cyprus": "CY",
    "Czech Republic": "CZ", "Denmark": "DK", "Egypt": "EG",
    "Estonia": "EE", "Finland": "FI", "France": "FR",
    "Georgia": "GE", "Germany": "DE", "Greece": "GR",
    "Hungary": "HU", "Iceland": "IS", "India": "IN",
    "Indonesia": "ID", "Ireland": "IE", "Israel": "IL",
    "Italy": "IT", "Japan": "JP", "Kazakhstan": "KZ",
    "Latvia": "LV", "Liechtenstein": "LI", "Lithuania": "LT",
    "Luxembourg": "LU", "Malaysia": "MY", "Malta": "MT",
    "Mexico": "MX", "Monaco": "MC", "Montenegro": "ME",
    "Morocco": "MA", "Netherlands": "NL", "New Zealand": "NZ",
    "Norway": "NO", "Philippines": "PH", "Poland": "PL",
    "Portugal": "PT", "Romania": "RO", "Russia": "RU",
    "Serbia": "RS", "Singapore": "SG", "Slovakia": "SK",
    "Slovenia": "SI", "South Africa": "ZA", "South Korea": "KR",
    "Spain": "ES", "Sweden": "SE", "Switzerland": "CH",
    "Taiwan": "TW", "Thailand": "TH", "Turkey": "TR",
    "Ukraine": "UA", "United Kingdom": "GB", "United States": "US",
    "Uruguay": "UY", "Venezuela": "VE",
}

# ---------------------------------------------------------------------
# RADIOID DATABASE
# ---------------------------------------------------------------------

RADIOID_BY_CALLSIGN = {}


def normalize_callsign(value):
    return str(value).strip().upper()


def empty_radioid_record():
    return {
        "match_count": 0,
        "id": "",
        "callsign": "",
        "name": "",
        "surname": "",
        "city": "",
        "state": "",
        "country": "",
        "country_code": ""
    }


def download_radioid_csv():
    os.makedirs(APP_DIR, exist_ok=True)

    temp_path = RADIOID_LOCAL_CSV + ".tmp"

    request = urllib.request.Request(
        RADIOID_CSV_URL,
        headers={"User-Agent": "HB9IIU-MMDVM-LiveBridge/1.0"}
    )

    with urllib.request.urlopen(request, timeout=RADIOID_DOWNLOAD_TIMEOUT_SECONDS) as response:
        with open(temp_path, "wb") as f:
            shutil.copyfileobj(response, f)

    if not os.path.isfile(temp_path):
        raise RuntimeError("temporary RadioID CSV file was not created")

    if os.path.getsize(temp_path) <= 0:
        raise RuntimeError("downloaded RadioID CSV is empty")

    os.replace(temp_path, RADIOID_LOCAL_CSV)
    print("RadioID CSV downloaded: %s" % RADIOID_LOCAL_CSV)


def ensure_radioid_csv_present():
    if os.path.isfile(RADIOID_LOCAL_CSV):
        return
    print("RadioID CSV not found, downloading...")
    download_radioid_csv()


def radioid_csv_is_stale():
    if not os.path.isfile(RADIOID_LOCAL_CSV):
        return True
    age_seconds = time.time() - os.path.getmtime(RADIOID_LOCAL_CSV)
    return age_seconds >= RADIOID_REFRESH_INTERVAL_SECONDS


def load_radioid_csv(filepath):
    global RADIOID_BY_CALLSIGN

    new_db = {}
    total_rows = 0

    with open(filepath, "r", encoding="utf-8", errors="replace", newline="") as f:
        reader = csv.reader(f)

        # Skip header row
        next(reader, None)

        for row in reader:
            total_rows += 1

            if len(row) < 7:
                continue

            radio_id = row[0].strip()
            callsign = normalize_callsign(row[1])
            first_name = row[2].strip()
            last_name = row[3].strip()
            city = row[4].strip()
            state = row[5].strip()
            country = row[6].strip()

            if not callsign:
                continue

            record = {
                "id": radio_id,
                "callsign": callsign,
                "name": first_name,
                "surname": last_name,
                "city": city,
                "state": state,
                "country": country,
                "country_code": COUNTRY_NAME_TO_ISO.get(country, "")
            }

            if callsign not in new_db:
                record["match_count"] = 1
                new_db[callsign] = record
            else:
                new_db[callsign]["match_count"] += 1

    RADIOID_BY_CALLSIGN = new_db
    SNAPSHOT_STATE["radioid_entries"] = len(RADIOID_BY_CALLSIGN)
    SNAPSHOT_STATE["radioid_csv_mtime"] = get_file_mtime(filepath)

    print("RadioID DB loaded: %d callsigns from %d CSV rows" % (len(RADIOID_BY_CALLSIGN), total_rows))


def lookup_callsign(callsign):
    return RADIOID_BY_CALLSIGN.get(normalize_callsign(callsign), empty_radioid_record())

# ---------------------------------------------------------------------
# CONFIG PARSING
# ---------------------------------------------------------------------

def clean_config_value(raw_value):
    value = raw_value.strip()

    if len(value) >= 2 and value.startswith('"') and value.endswith('"'):
        value = value[1:-1]

    return value


def parse_mmdvm_config_file():
    parser = configparser.ConfigParser(interpolation=None, strict=False)
    parser.optionxform = str

    with open(MMDVM_CONFIG_FILE, "r", encoding="utf-8", errors="replace") as f:
        parser.read_file(f)

    result = {}

    for section_name in parser.sections():
        result[section_name] = {}
        for key_name, raw_value in parser.items(section_name):
            result[section_name][key_name] = clean_config_value(raw_value)

    return result


def rebuild_snapshot_state():
    config_mtime = get_config_file_mtime()

    SNAPSHOT_STATE["service"]["state"] = get_service_state()
    SNAPSHOT_STATE["service"]["main_pid"] = get_service_main_pid()
    SNAPSHOT_STATE["service"]["active_since"] = get_service_active_since()
    SNAPSHOT_STATE["config_mtime"] = format_timestamp_utc(config_mtime)
    SNAPSHOT_STATE["config_mtime_ago_days"] = calculate_age_days(config_mtime)
    SNAPSHOT_STATE["current_log_file"] = find_latest_log_file()
    SNAPSHOT_STATE["config"] = parse_mmdvm_config_file()
    SNAPSHOT_STATE["radioid_csv_file"] = RADIOID_LOCAL_CSV
    SNAPSHOT_STATE["radioid_csv_mtime"] = get_file_mtime(RADIOID_LOCAL_CSV)

    station_callsign = SNAPSHOT_STATE["config"].get("General", {}).get("Callsign", "")
    station_info = lookup_callsign(station_callsign)

    SNAPSHOT_STATE["station_callsign"] = station_callsign
    SNAPSHOT_STATE["station_match_count"] = station_info["match_count"]
    SNAPSHOT_STATE["station_id"] = station_info["id"]
    SNAPSHOT_STATE["station_name"] = station_info["name"]
    SNAPSHOT_STATE["station_surname"] = station_info["surname"]
    SNAPSHOT_STATE["station_city"] = station_info["city"]
    SNAPSHOT_STATE["station_state"] = station_info["state"]
    SNAPSHOT_STATE["station_country"] = station_info["country"]
    SNAPSHOT_STATE["station_country_code"] = station_info["country_code"]

# ---------------------------------------------------------------------
# LIVE STATE HELPERS
# ---------------------------------------------------------------------

def parse_rssi_values(rssi_field):
    result = []

    for fragment in rssi_field.split("/"):
        cleaned = fragment.strip().replace(" dBm", "").replace("dBm", "")
        try:
            result.append(int(cleaned))
        except Exception:
            pass

    return result


def bump_event_id():
    LIVE_STATE["event_id"] += 1


def clear_source_fields():
    LIVE_STATE["source"] = ""
    LIVE_STATE["source_match_count"] = 0
    LIVE_STATE["source_id"] = ""
    LIVE_STATE["source_callsign"] = ""
    LIVE_STATE["source_name"] = ""
    LIVE_STATE["source_surname"] = ""
    LIVE_STATE["source_city"] = ""
    LIVE_STATE["source_state"] = ""
    LIVE_STATE["source_country"] = ""
    LIVE_STATE["source_country_code"] = ""


def apply_source_lookup(source_value):
    info = lookup_callsign(source_value)
    LIVE_STATE["source"] = source_value
    LIVE_STATE["source_match_count"] = info["match_count"]
    LIVE_STATE["source_id"] = info["id"]
    LIVE_STATE["source_callsign"] = info["callsign"]
    LIVE_STATE["source_name"] = info["name"]
    LIVE_STATE["source_surname"] = info["surname"]
    LIVE_STATE["source_city"] = info["city"]
    LIVE_STATE["source_state"] = info["state"]
    LIVE_STATE["source_country"] = info["country"]
    LIVE_STATE["source_country_code"] = info["country_code"]


def reset_live_state():
    LIVE_STATE["type"] = "live"
    LIVE_STATE["event_id"] = 0
    LIVE_STATE["timestamp"] = ""
    LIVE_STATE["mode"] = "Unknown"
    LIVE_STATE["last_event"] = ""
    LIVE_STATE["direction"] = ""
    LIVE_STATE["slot"] = None
    clear_source_fields()
    LIVE_STATE["destination"] = ""
    LIVE_STATE["talker_alias"] = ""
    LIVE_STATE["duration_sec"] = None
    LIVE_STATE["packet_loss_percent"] = None
    LIVE_STATE["ber_percent"] = None
    LIVE_STATE["rssi_values_dbm"] = []
    LIVE_STATE["raw_line"] = ""


def update_live_state_from_log_line(log_line):
    line = log_line.strip()
    if not line:
        return False

    match = REGEX_MODE_SET.match(line)
    if match:
        bump_event_id()
        LIVE_STATE["timestamp"] = match.group("timestamp")
        LIVE_STATE["mode"] = match.group("mode")
        LIVE_STATE["last_event"] = "mode_set"
        LIVE_STATE["direction"] = ""
        LIVE_STATE["slot"] = None
        clear_source_fields()
        LIVE_STATE["destination"] = ""
        LIVE_STATE["talker_alias"] = ""
        LIVE_STATE["duration_sec"] = None
        LIVE_STATE["packet_loss_percent"] = None
        LIVE_STATE["ber_percent"] = None
        LIVE_STATE["rssi_values_dbm"] = []
        LIVE_STATE["raw_line"] = line
        return True

    match = REGEX_RF_HEADER.match(line)
    if match:
        bump_event_id()
        source_value = match.group("source")
        LIVE_STATE["timestamp"] = match.group("timestamp")
        LIVE_STATE["last_event"] = "rf_voice_header"
        LIVE_STATE["direction"] = "rf"
        LIVE_STATE["slot"] = int(match.group("slot"))
        apply_source_lookup(source_value)
        LIVE_STATE["destination"] = match.group("destination")
        LIVE_STATE["talker_alias"] = ""
        LIVE_STATE["duration_sec"] = None
        LIVE_STATE["packet_loss_percent"] = None
        LIVE_STATE["ber_percent"] = None
        LIVE_STATE["rssi_values_dbm"] = []
        LIVE_STATE["raw_line"] = line
        return True

    match = REGEX_NETWORK_HEADER.match(line)
    if match:
        bump_event_id()
        source_value = match.group("source")
        LIVE_STATE["timestamp"] = match.group("timestamp")
        LIVE_STATE["last_event"] = "network_voice_header"
        LIVE_STATE["direction"] = "network"
        LIVE_STATE["slot"] = int(match.group("slot"))
        apply_source_lookup(source_value)
        LIVE_STATE["destination"] = match.group("destination")
        LIVE_STATE["talker_alias"] = ""
        LIVE_STATE["duration_sec"] = None
        LIVE_STATE["packet_loss_percent"] = None
        LIVE_STATE["ber_percent"] = None
        LIVE_STATE["rssi_values_dbm"] = []
        LIVE_STATE["raw_line"] = line
        return True

    match = REGEX_TALKER_ALIAS.match(line)
    if match:
        bump_event_id()
        LIVE_STATE["timestamp"] = match.group("timestamp")
        LIVE_STATE["last_event"] = "talker_alias"
        LIVE_STATE["slot"] = int(match.group("slot"))
        LIVE_STATE["talker_alias"] = match.group("alias")
        LIVE_STATE["raw_line"] = line
        return True

    match = REGEX_RF_END_LOST.match(line)
    if match:
        bump_event_id()
        LIVE_STATE["timestamp"] = match.group("timestamp")
        LIVE_STATE["last_event"] = "rf_voice_end_lost"
        LIVE_STATE["direction"] = "rf"
        LIVE_STATE["slot"] = int(match.group("slot"))
        apply_source_lookup(match.group("source"))
        LIVE_STATE["destination"] = match.group("destination")
        LIVE_STATE["duration_sec"] = float(match.group("duration"))
        LIVE_STATE["packet_loss_percent"] = None
        LIVE_STATE["ber_percent"] = float(match.group("ber"))
        LIVE_STATE["rssi_values_dbm"] = parse_rssi_values(match.group("rssi"))
        LIVE_STATE["raw_line"] = line
        return True

    match = REGEX_RF_END.match(line)
    if match:
        bump_event_id()
        LIVE_STATE["timestamp"] = match.group("timestamp")
        LIVE_STATE["last_event"] = "rf_voice_end"
        LIVE_STATE["direction"] = "rf"
        LIVE_STATE["slot"] = int(match.group("slot"))
        apply_source_lookup(match.group("source"))
        LIVE_STATE["destination"] = match.group("destination")
        LIVE_STATE["duration_sec"] = float(match.group("duration"))
        LIVE_STATE["packet_loss_percent"] = None
        LIVE_STATE["ber_percent"] = float(match.group("ber"))
        LIVE_STATE["rssi_values_dbm"] = parse_rssi_values(match.group("rssi"))
        LIVE_STATE["raw_line"] = line
        return True

    match = REGEX_NETWORK_END.match(line)
    if match:
        bump_event_id()
        LIVE_STATE["timestamp"] = match.group("timestamp")
        LIVE_STATE["last_event"] = "network_voice_end"
        LIVE_STATE["direction"] = "network"
        LIVE_STATE["slot"] = int(match.group("slot"))
        apply_source_lookup(match.group("source"))
        LIVE_STATE["destination"] = match.group("destination")
        LIVE_STATE["duration_sec"] = float(match.group("duration"))
        LIVE_STATE["packet_loss_percent"] = float(match.group("packet_loss"))
        LIVE_STATE["ber_percent"] = float(match.group("ber"))
        LIVE_STATE["rssi_values_dbm"] = []
        LIVE_STATE["raw_line"] = line
        return True

    return False

# ---------------------------------------------------------------------
# WEBSOCKET
# ---------------------------------------------------------------------

async def send_json(websocket, payload):
    await websocket.send(json.dumps(payload, separators=(",", ":")))


async def broadcast_live_state():
    if not CLIENTS:
        return

    dead_clients = []
    payload = copy.deepcopy(LIVE_STATE)

    for ws in CLIENTS:
        try:
            await send_json(ws, payload)
        except Exception:
            dead_clients.append(ws)

    for ws in dead_clients:
        CLIENTS.discard(ws)


async def broadcast_snapshot_state():
    if not CLIENTS:
        return

    dead_clients = []
    payload = copy.deepcopy(SNAPSHOT_STATE)

    for ws in CLIENTS:
        try:
            await send_json(ws, payload)
        except Exception:
            dead_clients.append(ws)

    for ws in dead_clients:
        CLIENTS.discard(ws)


async def ws_handler(websocket):
    CLIENTS.add(websocket)
    print("CLIENT CONNECTED")

    try:
        await send_json(websocket, copy.deepcopy(SNAPSHOT_STATE))
        await send_json(websocket, copy.deepcopy(LIVE_STATE))
        await websocket.wait_closed()
    finally:
        CLIENTS.discard(websocket)
        print("CLIENT DISCONNECTED")

# ---------------------------------------------------------------------
# BACKGROUND RADIOID REFRESH
# ---------------------------------------------------------------------

async def radioid_refresh_loop():
    while True:
        try:
            if radioid_csv_is_stale():
                print("RadioID CSV is stale, refreshing...")
                download_radioid_csv()
                load_radioid_csv(RADIOID_LOCAL_CSV)
                rebuild_snapshot_state()
                await broadcast_snapshot_state()
        except Exception as e:
            print("Warning: RadioID refresh failed: %s" % e)

        await asyncio.sleep(RADIOID_REFRESH_CHECK_INTERVAL_SECONDS)

# ---------------------------------------------------------------------
# LOG MONITOR
# ---------------------------------------------------------------------

async def monitor_log_forever():
    rebuild_snapshot_state()
    reset_live_state()

    ensure_radioid_csv_present()
    load_radioid_csv(RADIOID_LOCAL_CSV)
    rebuild_snapshot_state()

    current_log_file = SNAPSHOT_STATE["current_log_file"]
    if not current_log_file:
        raise RuntimeError("No MMDVM log file found")

    log_handle = open_log_file_at_end(current_log_file)

    last_config_file_mtime, last_service_main_pid, last_log_file = get_watch_state_tuple()
    last_recheck_time = time.monotonic()

    while True:
        line = log_handle.readline()

        if line:
            if update_live_state_from_log_line(line):
                print(json.dumps(LIVE_STATE, indent=2))
                await broadcast_live_state()
        else:
            await asyncio.sleep(LOG_POLL_INTERVAL_SECONDS)

        now_monotonic = time.monotonic()
        if now_monotonic - last_recheck_time < RECHECK_INTERVAL_SECONDS:
            continue

        last_recheck_time = now_monotonic

        current_config_file_mtime, current_service_main_pid, current_log_file = get_watch_state_tuple()

        config_changed = current_config_file_mtime != last_config_file_mtime
        pid_changed = current_service_main_pid != last_service_main_pid
        logfile_changed = current_log_file != last_log_file

        if config_changed or pid_changed or logfile_changed:
            print("CHANGE DETECTED")

            try:
                log_handle.close()
            except Exception:
                pass

            rebuild_snapshot_state()
            await broadcast_snapshot_state()

            current_log_file = SNAPSHOT_STATE["current_log_file"]
            if not current_log_file:
                raise RuntimeError("No MMDVM log file found after change")

            log_handle = open_log_file_at_end(current_log_file)

            last_config_file_mtime = current_config_file_mtime
            last_service_main_pid = current_service_main_pid
            last_log_file = current_log_file

# ---------------------------------------------------------------------
# MAIN
# ---------------------------------------------------------------------

async def main():
    rebuild_snapshot_state()
    reset_live_state()

    ensure_radioid_csv_present()
    load_radioid_csv(RADIOID_LOCAL_CSV)
    rebuild_snapshot_state()

    print("WebSocket server running on port %d" % WS_BIND_PORT)
    print("Current log file: %s" % SNAPSHOT_STATE["current_log_file"])
    print("RadioID CSV file: %s" % RADIOID_LOCAL_CSV)

    refresh_task = asyncio.create_task(radioid_refresh_loop())

    try:
        async with websockets.serve(ws_handler, WS_BIND_HOST, WS_BIND_PORT):
            await monitor_log_forever()
    finally:
        refresh_task.cancel()
        try:
            await refresh_task
        except asyncio.CancelledError:
            pass


if __name__ == "__main__":
    asyncio.run(main())