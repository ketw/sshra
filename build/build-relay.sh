#!/usr/bin/env bash
# =============================================================================
# build-relay.sh - Build kiro-relay for Linux/macOS VPS
# The relay server runs on any cloud VPS with a public IP.
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"
SRC="$ROOT/relay/relay.c"
INC="$ROOT/common"
OUT="$ROOT/build/kiro-relay"

echo ""
echo "  [*] Building kiro-relay"
echo "      Source : $SRC"
echo "      Output : $OUT"
echo ""

# Detect OS
OS="$(uname -s)"
EXTRA_LIBS=""
if [[ "$OS" == "Linux" ]]; then
    EXTRA_LIBS="-lpthread"
elif [[ "$OS" == "Darwin" ]]; then
    EXTRA_LIBS=""  # pthreads built-in on macOS
fi

gcc "$SRC" \
    -I"$INC" \
    -O2 -Wall -Wextra \
    -D_GNU_SOURCE \
    -o "$OUT" \
    $EXTRA_LIBS

echo ""
echo "  [OK] Built: $OUT"
echo ""
echo "  Run on your VPS:"
echo "    ./kiro-relay --port 7744 --token YOUR_SECRET_TOKEN"
echo ""
echo "  To run as a systemd service, see: deploy/kiro-relay.service"
