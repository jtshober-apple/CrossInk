#!/usr/bin/env bash
# CrossInk + Tesserae dev bootstrap
# Installs Claude Code, sets up git credentials, configures Tesserae in Docker,
# then drops you into a fully-briefed Claude Code session.
#
# Usage (run on your home server):
#   ANTHROPIC_API_KEY=sk-ant-... GH_TOKEN=ghp_... bash <(curl -fsSL \
#     https://raw.githubusercontent.com/jtshober-apple/CrossInk/main/scripts/dev-bootstrap.sh)
#
# Required env vars:
#   ANTHROPIC_API_KEY  Your Anthropic API key (from console.anthropic.com)
#   GH_TOKEN           GitHub PAT with repo write scope

set -euo pipefail

: "${ANTHROPIC_API_KEY:?Set ANTHROPIC_API_KEY=sk-ant-... before running}"
: "${GH_TOKEN:?Set GH_TOKEN=ghp_... before running}"

GH_USER="jtshober-apple"
REPO_NAME="CrossInk"
WORKDIR="$HOME/crossink-dev"

banner() { echo; echo "==> $*"; }

# ── Prerequisites ─────────────────────────────────────────────────────────────

banner "Checking prerequisites..."

if ! command -v node &>/dev/null; then
  banner "Installing Node.js (LTS)..."
  if command -v apt-get &>/dev/null; then
    curl -fsSL https://deb.nodesource.com/setup_lts.x | sudo -E bash -
    sudo apt-get install -y nodejs
  elif command -v dnf &>/dev/null; then
    sudo dnf install -y nodejs
  else
    echo "ERROR: Cannot detect package manager. Install Node.js manually then re-run." >&2
    exit 1
  fi
fi
echo "Node.js $(node --version)"

if ! command -v claude &>/dev/null; then
  banner "Installing Claude Code..."
  sudo npm install -g @anthropic-ai/claude-code
fi
echo "Claude Code $(claude --version 2>/dev/null || true)"

if ! command -v docker &>/dev/null; then
  echo "WARNING: docker not found — skipping Tesserae setup. Install Docker and re-run if you want Tesserae now."
  SKIP_DOCKER=1
else
  SKIP_DOCKER=0
fi

# ── Git credentials ───────────────────────────────────────────────────────────

banner "Configuring git..."
git config --global user.email "jacob.shober@gmail.com"
git config --global user.name "Jacob Shober"
git config --global credential.helper store
printf 'https://%s:%s@github.com\n' "$GH_USER" "$GH_TOKEN" > ~/.git-credentials
chmod 600 ~/.git-credentials
echo "Git credentials stored."

# ── Clone / update repo ───────────────────────────────────────────────────────

banner "Syncing CrossInk repo..."
REPO_URL="https://${GH_USER}:${GH_TOKEN}@github.com/${GH_USER}/${REPO_NAME}.git"
if [ -d "$WORKDIR/.git" ]; then
  git -C "$WORKDIR" pull
  echo "Pulled latest into $WORKDIR"
else
  git clone "$REPO_URL" "$WORKDIR"
  echo "Cloned into $WORKDIR"
fi
cd "$WORKDIR"

# ── Tesserae Docker ───────────────────────────────────────────────────────────

if [ "$SKIP_DOCKER" -eq 0 ]; then
  banner "Setting up Tesserae (standalone, no Home Assistant)..."
  sudo mkdir -p /opt/docker/tesserae/data

  if [ ! -f /opt/docker/tesserae/docker-compose.yml ]; then
    SECRET=$(python3 -c "import secrets; print(secrets.token_hex(32))")
    sudo tee /opt/docker/tesserae/docker-compose.yml > /dev/null <<COMPOSE
services:
  tesserae:
    image: ghcr.io/dmellok/tesserae:latest
    container_name: tesserae
    restart: unless-stopped
    network_mode: host
    environment:
      TESSERAE_SECRET_KEY: "${SECRET}"
      TESSERAE_IN_DOCKER: "1"
    volumes:
      - ./data:/app/data
COMPOSE
    echo "docker-compose.yml written (secret key generated)."
  else
    echo "docker-compose.yml already exists — skipping."
  fi

  cd /opt/docker/tesserae
  sudo docker compose up -d
  SERVER_IP=$(hostname -I | awk '{print $1}')
  echo "Tesserae running at http://${SERVER_IP}:8765"
  cd "$WORKDIR"
else
  SERVER_IP=$(hostname -I | awk '{print $1}' 2>/dev/null || echo "YOUR_SERVER_IP")
fi

# ── Launch Claude Code with full context ──────────────────────────────────────

banner "Launching Claude Code with Tesserae integration context..."

read -r -d '' CONTEXT_PROMPT <<'PROMPT' || true
You are continuing CrossInk firmware development for the jtshober-apple/CrossInk fork.
The repo is already cloned at the current directory. GitHub PAT for pushing is already in
~/.git-credentials, so plain `git push` works.

── GOAL ─────────────────────────────────────────────────────────────────────────
Add a Tesserae dashboard client to CrossInk so the X4 Pro device displays a
Tesserae widget dashboard (a server-rendered BMP) as its e-ink sleep screen.
CrossInk stays the primary reader firmware; Tesserae provides the dashboard layer.

── TESSERAE REST PROTOCOL ───────────────────────────────────────────────────────
Tesserae runs at http://SERVER_IP:8765 (Docker on user's home server; no HA).
Auth header on all requests: X-Tesserae-Token: <token>

1. First boot / pairing — POST /api/v1/device/discover
   Body (JSON): { "mac": "<MAC>", "firmware_version": "crossink-1.x",
                  "width": 800, "height": 480, "color_depth": 1 }
   Response 200: { "token": "<device_token>", "device_id": "<id>" }  ← save both to settings
   Response 202: waiting for admin to approve in Tesserae dashboard (poll again in 60 s)

2. Frame fetch — GET /api/v1/device/<id>/frame.bmp
   Header: If-None-Match: <last_etag>   (skip repaint on 304)
   Response 200: BMP binary + ETag response header  ← draw on e-ink, cache etag
   Response 204: no dashboard assigned yet
   Response 304: frame unchanged  ← skip repaint

3. Heartbeat — POST /api/v1/device/<id>/status
   Body (JSON): { "battery_pct": <int>, "rssi": <int>, "ip": "<ip>",
                  "firmware_version": "crossink-1.x" }
   Response 200: { "next_poll_s": <int>, "server_time": <iso>, "config": {} }
   next_poll_s tells the device how long to deep-sleep before next poll.

── WHAT TO BUILD ────────────────────────────────────────────────────────────────
All new files go in src/activities/network/ alongside existing network activities.

1. src/activities/network/TesseraeActivity.h/.cpp
   - Inherits Activity ("Tesserae", renderer, mappedInput)
   - States: DISCOVER → AWAITING_APPROVAL → FETCH_FRAME → SHOWING → SLEEP
   - Sends discover on first run, polls /frame.bmp with ETag, posts /status heartbeat
   - Uses freeink-sdk HTTP client (look at how KOReaderSyncActivity fetches — same pattern)
   - Renders received BMP to framebuffer using renderer.drawBitmap() or equivalent
   - Stores next_poll_s; SleepActivity reads it to set the deep-sleep duration
   - Settings stored in SETTINGS (CrossPointSettings): tessServerUrl, tessDeviceId,
     tessDeviceToken, tessEtag (all std::string)

2. lib/I18n/translations/english.yaml — add:
   STR_TESSERAE: "Tesserae Dashboard"
   STR_TESSERAE_SERVER_URL: "Server URL"
   STR_TESSERAE_PAIRING: "Waiting for admin approval..."
   STR_TESSERAE_NO_FRAME: "No dashboard assigned"
   Then regenerate: python3 scripts/gen_i18n.py

3. src/CrossPointSettings.h — add fields (near the KOSync block):
   std::string tessServerUrl;
   std::string tessDeviceId;
   std::string tessDeviceToken;
   std::string tessEtag;

4. src/SettingsList.cpp — register tessServerUrl as a keyboard-entry setting under a
   new STR_TESSERAE section (look at how STR_KOREADER_SYNC settings are grouped).

5. src/activities/boot_sleep/SleepActivity.cpp — when SETTINGS.tessDeviceToken is
   non-empty, launch TesseraeActivity instead of (or before) the static sleep image.

6. CHANGELOG.md — add an "Added" entry under the current unreleased section.

── CROSSINK ARCHITECTURE QUICK-REF ─────────────────────────────────────────────
- Activities: heap-allocated, owned by ActivityManager unique_ptr.
  Resources in onEnter(), freed in onExit(). Use startActivityForResult() for sub-flows.
- Settings: SETTINGS macro → CrossPointSettings. Persist with SETTINGS.saveToFile().
  Toggle fields must be uint8_t, not bool.
- Strings: tr(STR_*) for all UI text. Add keys to english.yaml, run gen_i18n.py.
- HAL: use lib/hal/ wrappers. SecureNet / HTTP client is in freeink-sdk.
- Build: NO local toolchain. Push to main → trigger GitHub Actions build_x4pro.yml
  (workflow_dispatch). Check results at:
  https://github.com/jtshober-apple/CrossInk/actions
- Memory: new (std::nothrow) or makeUniqueNoThrow<T>(). No bare new — it aborts on OOM.
- Error handling: LOG_ERR(...) + return false. No exceptions.
- All server files in /opt/docker: write with sudo tee, not cat >.
  All docker commands need sudo on this server.

── WHERE TO START ───────────────────────────────────────────────────────────────
1. Read src/activities/network/KOReaderSyncActivity.h to understand the network
   activity pattern (HTTP, task, callbacks).
2. Read src/activities/boot_sleep/SleepActivity.cpp to understand the sleep render flow.
3. Read src/CrossPointSettings.h lines ~100-160 for the KOSync fields to copy the pattern.
4. Build incrementally: add settings fields → write TesseraeActivity skeleton → wire
   into sleep → add discover/fetch/heartbeat → commit & push each logical piece.
5. After each push, trigger build: gh workflow run build_x4pro.yml --repo jtshober-apple/CrossInk

Ready. Start with: read src/activities/network/KOReaderSyncActivity.h
PROMPT

# Replace placeholder with actual server IP
CONTEXT_PROMPT="${CONTEXT_PROMPT//SERVER_IP/${SERVER_IP}}"

export ANTHROPIC_API_KEY
claude "$CONTEXT_PROMPT"
