# AI Usage Tracker — Backend

Minimal Node.js server that serves Claude subscription quota and (optionally) Codex session usage to the TTGO T-Display ESP32 firmware.

## Endpoints

| Endpoint | Description |
|---|---|
| `GET /api/claude/usage` | Claude session % + weekly quota + reset countdowns |
| `GET /api/codex/usage` | Codex session turns/tokens (requires `CODEX_TRAJECTORY_DIR`) |
| `GET /health` | Health check |

## Quick Start — Docker (recommended)

```bash
# 1. Copy env file
cp .env.example .env

# 2. Add your Claude token
#    After `claude login`, grab accessToken from ~/.claude/.credentials.json
echo "CLAUDE_OAUTH_TOKEN=your_token_here" > .env

# 3. Start
docker compose up -d

# 4. Verify
curl http://localhost:8030/api/claude/usage
```

The server binds to `0.0.0.0:8030` — accessible from devices on your local network.

## Quick Start — Node (no Docker)

```bash
npm install
CLAUDE_OAUTH_TOKEN=your_token node server.js
```

## Codex Usage (optional)

If you use OpenClaw's Codex integration, mount your agents directory and set `CODEX_TRAJECTORY_DIR`:

```yaml
# docker-compose.yml
volumes:
  - ~/.openclaw/agents:/data/agents:ro
environment:
  - CODEX_TRAJECTORY_DIR=/data/agents
```

Without this, `/api/codex/usage` returns a 404 and the device falls back to Claude-only display.

## Firmware Configuration

The ESP32 firmware needs to know where this server is running.

On first boot, the device shows a setup screen with its IP address after WiFi connects. Visit `http://<device-ip>/config` in a browser to set the server host and port. Settings are saved to flash and persist across reboots.

To reset to defaults, hold the BOOT button at startup (re-provisions WiFi + server config).

## Environment Variables

| Variable | Required | Default | Description |
|---|---|---|---|
| `CLAUDE_OAUTH_TOKEN` | Yes | — | Claude OAuth access token |
| `CODEX_TRAJECTORY_DIR` | No | — | Path to OpenClaw agents dir |
| `PORT` | No | `8030` | HTTP port |
