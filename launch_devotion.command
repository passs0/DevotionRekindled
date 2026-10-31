#!/bin/zsh
# Devotion DualSense Wireless Launcher
# Presents your wireless Bluetooth DualSense as a wired DualShock 4 to Devotion
# without modifying any files in Devotion.app.

DIR="$(cd "$(dirname "$0")" && pwd)"
SHIM="$DIR/libds4_shim.dylib"
GAME="/Applications/Devotion.app/Contents/MacOS/Devotion"

echo "=========================================================="
echo "  Devotion - Wireless DualSense to DualShock 4 Bridge"
echo "=========================================================="
echo "Shim: $SHIM"
echo "Game: $GAME"
echo "----------------------------------------------------------"
echo "Make sure your DualSense controller is connected via Bluetooth."
echo "Starting game now..."
echo ""

if [ ! -f "$SHIM" ]; then
    echo "Error: $SHIM not found!"
    read -k 1 -s "?Press any key to exit..."
    exit 1
fi

if [ ! -f "$GAME" ]; then
    echo "Error: Devotion not found at $GAME!"
    read -k 1 -s "?Press any key to exit..."
    exit 1
fi

DYLD_INSERT_LIBRARIES="$SHIM" "$GAME" "$@"
