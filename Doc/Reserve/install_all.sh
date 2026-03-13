#!/bin/bash

cat <<'EOF'
       __  __ __  __ ______     ____  __         
      |  \/  |  \/  |  _ \ \   / /  \/  |        
      | |\/| | |\/| | | | \ \ / /| |\/| |        
      | |  | | |  | | |_| |\ V / | |  | |        
 _    |_|  |_|_|  |_|____/  \_/  |_|_ |_|        
| |   (_)_   _____| __ ) _ __(_) __| | __ _  ___ 
| |   | \ \ / / _ \  _ \| '__| |/ _` |/ _` |/ _ \
| |___| |\ V /  __/ |_) | |  | | (_| | (_| |  __/
|_____|_| \_/ \___|____/|_|  |_|\__,_|\__, |\___|
                                      |___/      
EOF

set -euo pipefail

# =========================================================
# ESP32-PI-STAR-CLIENT — Full installer
# =========================================================

GITHUB_RAW="https://raw.githubusercontent.com/HB9IIU/ESP32-PI-STAR-CLIENT/main/InstallationFiles"
PY_SCRIPT="/home/pi-star/monitor_mmdvm_ws.py"
SERVICE_NAME="monitor_mmdvm_ws.service"
SERVICE_INSTALLER_URL="${GITHUB_RAW}/install_mmdvm_ws_service.sh"
WS_PORT="8765"
PINNED_WEBSOCKETS_VERSION="13.1"

APT_SOURCES_MAIN="/etc/apt/sources.list"
APT_SOURCES_RASPI="/etc/apt/sources.list.d/raspi.list"
APT_SOURCES_BACKPORTS="/etc/apt/sources.list.d/bullseye-backports.list"

PISTAR_FIREWALL_SCRIPT="/usr/local/sbin/pistar-firewall"
PISTAR_FIREWALL_RULE="iptables -A INPUT -p tcp --dport ${WS_PORT} -j ACCEPT #                    MMDVM WebSocket Monitor"

TOTAL_STEPS=13

echo "========================================================="
echo " 🚀 ESP32-PI-STAR-CLIENT — Full installer"
echo "========================================================="

# ---------------------------------------------------------
# Helpers
# ---------------------------------------------------------

step() {
    echo
    echo "[$1/${TOTAL_STEPS}] $2"
}

ok() {
    echo "      ✔ $1"
}

info() {
    echo "      ➜ $1"
}

warn() {
    echo "      ⚠ $1"
}

die() {
    echo "      ✘ ERROR: $1"
    exit 1
}

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
}

# ---------------------------------------------------------
# [1/13] Environment checks
# ---------------------------------------------------------
step 1 "🔎 Checking environment..."

require_cmd python3
require_cmd curl
require_cmd sed
require_cmd grep
require_cmd systemctl
require_cmd iptables
require_cmd apt

PYVER="$(python3 -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}.{sys.version_info.micro}")')"
ok "Python version: ${PYVER}"

if [ -f /etc/os-release ]; then
    . /etc/os-release
    ok "OS: ${PRETTY_NAME:-unknown}"
else
    warn "/etc/os-release not found"
fi

ok "User: $(id -un)"
ok "Target script: ${PY_SCRIPT}"
ok "Service: ${SERVICE_NAME}"
ok "WebSocket port: ${WS_PORT}"

# ---------------------------------------------------------
# [2/13] Switch filesystem to read-write
# ---------------------------------------------------------
step 2 "🛠️ Switching filesystem to read-write..."

if command -v rpi-rw >/dev/null 2>&1; then
    if rpi-rw >/dev/null 2>&1; then
        ok "filesystem switched to read-write with rpi-rw"
    else
        sudo mount -o remount,rw / || die "could not remount / as read-write"
        ok "filesystem remounted read-write"
    fi
else
    sudo mount -o remount,rw / || die "could not remount / as read-write"
    ok "filesystem remounted read-write"
fi

# ---------------------------------------------------------
# [3/13] Repair APT sources if needed
# ---------------------------------------------------------
step 3 "📦 Checking and repairing APT sources if needed..."

if [ -f "${APT_SOURCES_MAIN}" ] && grep -q 'legacy.raspbian.org/raspbian' "${APT_SOURCES_MAIN}"; then
    sudo sed -i 's#https://legacy.raspbian.org/raspbian/#http://archive.raspbian.org/raspbian/#g' "${APT_SOURCES_MAIN}"
    sudo sed -i 's#http://legacy.raspbian.org/raspbian/#http://archive.raspbian.org/raspbian/#g' "${APT_SOURCES_MAIN}"
    ok "fixed Raspbian archive URL in ${APT_SOURCES_MAIN}"
else
    ok "main Raspbian source already OK"
fi

if [ -f "${APT_SOURCES_BACKPORTS}" ]; then
    sudo rm -f "${APT_SOURCES_BACKPORTS}"
    ok "removed dead bullseye-backports entry"
else
    ok "no dead bullseye-backports entry found"
fi

if [ -f "${APT_SOURCES_RASPI}" ]; then
    ok "raspi.list present"
else
    warn "raspi.list not found at ${APT_SOURCES_RASPI}"
fi

# ---------------------------------------------------------
# [4/13] Refresh package lists
# ---------------------------------------------------------
step 4 "🔄 Refreshing APT package lists..."

sudo apt update || die "apt update failed"
ok "apt update completed"

# ---------------------------------------------------------
# [5/13] Install minimum OS prerequisites
# ---------------------------------------------------------
step 5 "🧰 Installing minimum OS prerequisites..."

sudo apt install -y python3-pip || die "could not install python3-pip"
ok "python3-pip installed"

# ---------------------------------------------------------
# [6/13] Ensure pinned websockets version
# ---------------------------------------------------------
step 6 "🐍 Checking websockets installation..."

CURRENT_WS_VERSION=""

if python3 -c "import websockets" >/dev/null 2>&1; then
    CURRENT_WS_VERSION="$(python3 -c 'import websockets; print(websockets.__version__)' 2>/dev/null || true)"
fi

if [ "${CURRENT_WS_VERSION}" = "${PINNED_WEBSOCKETS_VERSION}" ]; then
    ok "websockets already installed (${PINNED_WEBSOCKETS_VERSION})"
else
    if [ -n "${CURRENT_WS_VERSION}" ]; then
        info "found incompatible websockets version: ${CURRENT_WS_VERSION}"
        sudo python3 -m pip uninstall -y websockets >/dev/null 2>&1 || true
        ok "removed previous websockets version"
    else
        info "websockets not currently installed"
    fi

    sudo python3 -m pip install --no-cache-dir "websockets==${PINNED_WEBSOCKETS_VERSION}" || die "failed to install websockets ${PINNED_WEBSOCKETS_VERSION}"
    ok "installed websockets ${PINNED_WEBSOCKETS_VERSION}"
fi

WS_VER="$(python3 -c 'import websockets; print(websockets.__version__)')"
WS_FILE="$(python3 -c 'import websockets; print(websockets.__file__)')"

[ "${WS_VER}" = "${PINNED_WEBSOCKETS_VERSION}" ] || die "unexpected websockets version after install: ${WS_VER}"
ok "verified websockets version: ${WS_VER}"
ok "websockets path: ${WS_FILE}"

# ---------------------------------------------------------
# [7/13] Download monitor Python script
# ---------------------------------------------------------
step 7 "⬇️ Downloading monitor_mmdvm_ws.py..."

sudo mkdir -p "$(dirname "${PY_SCRIPT}")"
curl -fsSL "${GITHUB_RAW}/monitor_mmdvm_ws.py" -o /tmp/monitor_mmdvm_ws.py || die "failed to download monitor_mmdvm_ws.py"
sudo mv /tmp/monitor_mmdvm_ws.py "${PY_SCRIPT}"
sudo chmod 755 "${PY_SCRIPT}"
ok "downloaded monitor script to ${PY_SCRIPT}"

# ---------------------------------------------------------
# [8/13] Apply sanity fixes if needed
# ---------------------------------------------------------
step 8 "🩹 Applying sanity fixes if needed..."

if grep -q 'split(";")' "${PY_SCRIPT}"; then
    sudo sed -i 's/split(";")/split("\\t")/g' "${PY_SCRIPT}"
    ok "patched DMR ID separator from semicolon to tab"
else
    ok "DMR ID separator already looks OK"
fi

if [ -f /usr/local/etc/DMRIds.dat ]; then
    ok "DMRIds.dat found"
else
    warn "DMRIds.dat not found at /usr/local/etc/DMRIds.dat"
fi

# ---------------------------------------------------------
# [9/13] Add live firewall rule and stop old instances
# ---------------------------------------------------------
step 9 "🌐 Checking live firewall rule and stopping old instances..."

if sudo iptables -C INPUT -p tcp --dport "${WS_PORT}" -j ACCEPT 2>/dev/null; then
    ok "live firewall rule already present"
else
    sudo iptables -I INPUT 1 -p tcp --dport "${WS_PORT}" -j ACCEPT
    ok "live firewall rule added for port ${WS_PORT}"
fi

if pgrep -f "python3 ${PY_SCRIPT}" >/dev/null 2>&1; then
    sudo pkill -f "python3 ${PY_SCRIPT}" || true
    ok "stopped running standalone monitor instance"
else
    ok "no running standalone monitor instance found"
fi

# ---------------------------------------------------------
# [10/13] Add permanent Pi-Star firewall rule
# ---------------------------------------------------------
step 10 "🧱 Adding permanent Pi-Star firewall rule..."

if [ ! -f "${PISTAR_FIREWALL_SCRIPT}" ]; then
    warn "${PISTAR_FIREWALL_SCRIPT} not found; skipping permanent firewall integration"
else
    if grep -Fq -- "--dport ${WS_PORT}" "${PISTAR_FIREWALL_SCRIPT}"; then
        ok "permanent Pi-Star firewall rule already present"
    else
        sudo sed -i "/iptables -A INPUT -p tcp --dport 443 -j ACCEPT/a ${PISTAR_FIREWALL_RULE}" "${PISTAR_FIREWALL_SCRIPT}" \
            || die "failed to insert permanent Pi-Star firewall rule"
        ok "permanent Pi-Star firewall rule inserted into ${PISTAR_FIREWALL_SCRIPT}"
    fi
fi

# ---------------------------------------------------------
# [11/13] Apply Pi-Star firewall
# ---------------------------------------------------------
step 11 "🔥 Applying Pi-Star firewall..."

if [ -f "${PISTAR_FIREWALL_SCRIPT}" ]; then
    sudo pistar-firewall || die "failed to apply Pi-Star firewall"
    ok "Pi-Star firewall applied"
else
    warn "Pi-Star firewall script not found; only live iptables rule is active"
fi

# ---------------------------------------------------------
# [12/13] Install or refresh service
# ---------------------------------------------------------
step 12 "⚙️ Installing systemd service..."

SERVICE_FILE="/etc/systemd/system/${SERVICE_NAME}"

sudo tee "${SERVICE_FILE}" >/dev/null <<EOF
[Unit]
Description=MMDVM WebSocket Monitor
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=pi-star
WorkingDirectory=/home/pi-star
ExecStart=/usr/bin/python3 ${PY_SCRIPT}
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

ok "service file written to ${SERVICE_FILE}"

sudo chmod 644 "${SERVICE_FILE}" || die "failed to chmod service file"
sudo systemctl daemon-reload || die "systemctl daemon-reload failed"
sudo systemctl enable "${SERVICE_NAME}" || die "failed to enable ${SERVICE_NAME}"
sudo systemctl restart "${SERVICE_NAME}" || die "failed to restart ${SERVICE_NAME}"
ok "service restarted"

# ---------------------------------------------------------
# [13/13] Final verification
# ---------------------------------------------------------
step 13 "✅ Final verification..."

if systemctl is-active --quiet "${SERVICE_NAME}"; then
    ok "service is active"
else
    warn "service is not active"
fi

if sudo ss -ltnp 2>/dev/null | grep -q ":${WS_PORT} "; then
    ok "port ${WS_PORT} is listening"
else
    warn "port ${WS_PORT} is not currently listening"
fi

if [ -f "${PISTAR_FIREWALL_SCRIPT}" ] && grep -Fq -- "--dport ${WS_PORT}" "${PISTAR_FIREWALL_SCRIPT}"; then
    ok "permanent Pi-Star firewall rule for port ${WS_PORT} is present"
else
    warn "permanent Pi-Star firewall rule for port ${WS_PORT} is missing"
fi

echo
echo "========================================================="
echo " ✅ Installation complete"
echo "========================================================="
echo " Python      : ${PYVER}"
echo " websockets  : ${WS_VER}"
echo " script      : ${PY_SCRIPT}"
echo " service     : ${SERVICE_NAME}"
echo " port        : ${WS_PORT}"
echo
echo " Useful commands:"
echo "   sudo systemctl status ${SERVICE_NAME} --no-pager"
echo "   journalctl -u ${SERVICE_NAME} -f"
echo "   python3 ${PY_SCRIPT}"
echo "========================================================="