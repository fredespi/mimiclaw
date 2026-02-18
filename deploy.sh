#!/usr/bin/env bash
# MimiClaw — wireless OTA deploy
# Builds firmware, serves it over HTTP, triggers OTA, waits for device callback.
set -e

# ── Config ──────────────────────────────────────────────────────
SERVE_PORT="${SERVE_PORT:-8199}"
DEVICE_IP="${DEVICE_IP:-192.168.1.216}"
DEVICE_PORT="${DEVICE_PORT:-18789}"
BIN_FILE="build/mimiclaw.bin"
TIMEOUT="${OTA_TIMEOUT:-120}"  # seconds to wait for callback

# ── Find local IP ──────────────────────────────────────────────
get_local_ip() {
    if command -v ipconfig &>/dev/null; then
        local ip
        for iface in en0 en1; do
            ip=$(ipconfig getifaddr "$iface" 2>/dev/null) && [ -n "$ip" ] && echo "$ip" && return
        done
    fi
    hostname -I 2>/dev/null | awk '{print $1}' && return
    echo "127.0.0.1"
}

LOCAL_IP=$(get_local_ip)
OTA_URL="http://${LOCAL_IP}:${SERVE_PORT}/mimiclaw.bin"

echo "=== MimiClaw OTA Deploy ==="
echo "Local IP:  ${LOCAL_IP}"
echo "Device:    ${DEVICE_IP}:${DEVICE_PORT}"
echo "OTA URL:   ${OTA_URL}"
echo ""

# ── Step 1: Build ──────────────────────────────────────────────
echo "▸ Building firmware..."
source "$HOME/.espressif/v5.5.2/esp-idf/export.sh" 2>/dev/null
idf.py build

if [ ! -f "$BIN_FILE" ]; then
    echo "ERROR: $BIN_FILE not found"
    exit 1
fi

BIN_SIZE=$(stat -f%z "$BIN_FILE" 2>/dev/null || stat -c%s "$BIN_FILE" 2>/dev/null)
echo "▸ Firmware: ${BIN_FILE} (${BIN_SIZE} bytes)"
echo ""

# ── Step 2: Start OTA server (serves binary + waits for callback) ──
echo "▸ Starting OTA server on port ${SERVE_PORT}..."

# Python server: serves files on GET, exits on POST /ota_done
python3 -c "
import http.server, json, sys, threading, os

os.chdir('build')

class OTAHandler(http.server.SimpleHTTPRequestHandler):
    def do_POST(self):
        if self.path == '/ota_done':
            length = int(self.headers.get('Content-Length', 0))
            body = self.rfile.read(length).decode() if length else '{}'
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.end_headers()
            self.wfile.write(b'{\"status\":\"ok\"}')
            print(flush=True)
            try:
                info = json.loads(body)
                ver = info.get('version', '?')
                print(f'\\n=== OTA COMPLETE ===', flush=True)
                print(f'Device rebooted with version: {ver}', flush=True)
            except:
                print(f'\\n=== OTA COMPLETE ===', flush=True)
            # Schedule shutdown
            threading.Thread(target=lambda: (
                __import__('time').sleep(0.5),
                os._exit(0)
            ), daemon=True).start()
        else:
            self.send_error(404)
    def log_message(self, format, *args):
        print(f'  [{self.address_string()}] {format % args}', flush=True)

server = http.server.HTTPServer(('0.0.0.0', ${SERVE_PORT}), OTAHandler)
server.timeout = ${TIMEOUT}
print(f'  Serving on port ${SERVE_PORT} (timeout: ${TIMEOUT}s)', flush=True)
server.serve_forever()
" &
SERVER_PID=$!

cleanup() {
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
}
trap cleanup EXIT

sleep 1

# ── Step 3: POST OTA URL to device ───────────────────────────
echo "▸ Sending OTA command to device..."
HTTP_CODE=$(curl -s -o /tmp/ota_resp.json -w "%{http_code}" \
    -X POST "http://${DEVICE_IP}:${DEVICE_PORT}/ota" \
    -H "Content-Type: application/json" \
    -d "{\"url\":\"${OTA_URL}\"}" \
    --connect-timeout 5 --max-time 10)

if [ "$HTTP_CODE" = "200" ]; then
    echo "▸ Device acknowledged. Downloading firmware..."
else
    echo "ERROR: Device returned HTTP ${HTTP_CODE}"
    cat /tmp/ota_resp.json 2>/dev/null; echo ""
    echo "  DEVICE_IP=${DEVICE_IP} ./deploy.sh"
    exit 1
fi

# ── Step 4: Wait for callback ────────────────────────────────
echo "▸ Waiting for device to reboot and call back (timeout: ${TIMEOUT}s)..."
wait "$SERVER_PID" 2>/dev/null
EXIT_CODE=$?

if [ "$EXIT_CODE" -eq 0 ]; then
    echo ""
    echo "▸ Deploy finished successfully!"
else
    echo ""
    echo "⚠ Server exited with code ${EXIT_CODE}. Device may still be updating."
fi
