#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export NATSUKI_SHIMEJI_HOME="$ROOT"
pkill -f "^/.*/arch-linux-ascii-wallpaper" 2>/dev/null || true
pkill -f "^/.*/natsuki-ascii-wallpaper" 2>/dev/null || true
sleep 0.12
exec "$ROOT/arch-linux-ascii-wallpaper" "$@"

