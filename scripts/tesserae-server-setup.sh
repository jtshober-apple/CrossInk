#!/usr/bin/env bash
# CrossInk Tesserae Dashboard Server Setup
# Run this on your home server (as a user with sudo access):
#   bash <(curl -fsSL https://raw.githubusercontent.com/jtshober-apple/CrossInk/main/scripts/tesserae-server-setup.sh)
#
# What it does:
#   1. Creates /opt/docker/tesserae/ if it doesn't exist.
#   2. Writes a docker-compose.yml that starts Tesserae on port 8765.
#   3. Pulls the latest Tesserae image and starts the container.
#   4. Prints the URL to paste into your CrossInk device settings.

set -euo pipefail

INSTALL_DIR="/opt/docker/tesserae"
COMPOSE_FILE="$INSTALL_DIR/docker-compose.yml"
PORT="${TESSERAE_PORT:-8765}"

# ── helpers ──────────────────────────────────────────────────────────────────
info()  { echo "[tesserae-setup] $*"; }
error() { echo "[tesserae-setup] ERROR: $*" >&2; exit 1; }

# ── prerequisites ─────────────────────────────────────────────────────────────
command -v docker >/dev/null 2>&1 || error "Docker is not installed. Install it first: https://docs.docker.com/engine/install/"
docker compose version >/dev/null 2>&1 || error "Docker Compose plugin not found. Install it: https://docs.docker.com/compose/install/"

# ── create directory ──────────────────────────────────────────────────────────
info "Creating $INSTALL_DIR ..."
sudo mkdir -p "$INSTALL_DIR"

# ── write docker-compose.yml ──────────────────────────────────────────────────
info "Writing $COMPOSE_FILE ..."
sudo tee "$COMPOSE_FILE" > /dev/null <<COMPOSE
services:
  tesserae:
    image: ghcr.io/dmellok/tesserae:latest
    container_name: tesserae
    ports:
      - "${PORT}:8765"
    volumes:
      - tesserae_data:/app/data
    restart: unless-stopped
    environment:
      - TESSERAE_PORT=8765

volumes:
  tesserae_data:
COMPOSE

# ── pull and start ─────────────────────────────────────────────────────────────
info "Pulling latest Tesserae image ..."
sudo docker compose -f "$COMPOSE_FILE" pull

info "Starting Tesserae ..."
sudo docker compose -f "$COMPOSE_FILE" up -d

# ── discover server IP ────────────────────────────────────────────────────────
SERVER_IP="$(hostname -I 2>/dev/null | awk '{print $1}')"
if [[ -z "$SERVER_IP" ]]; then
  SERVER_IP="<your-server-ip>"
fi

echo ""
echo "============================================================"
echo "  Tesserae is running on port $PORT"
echo ""
echo "  Admin UI:  http://${SERVER_IP}:${PORT}/admin"
echo ""
echo "  CrossInk device setting (Settings → Tesserae → Server URL):"
echo "    http://${SERVER_IP}:${PORT}"
echo ""
echo "  First time: open the admin UI and approve your device after"
echo "  tapping 'Sync Dashboard Now' on your CrossInk."
echo "============================================================"
