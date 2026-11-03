#!/bin/zsh
set -e

# Locate the folder where this .command script lives
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

SAVE_SRC="$SCRIPT_DIR/inc.redcandlegames.devotion"
PLIST_SRC="$SCRIPT_DIR/inc.redcandlegames.devotion.plist"

CONTAINER_LIB="$HOME/Library/Containers/inc.redcandlegames.devotion/Data/Library"
APP_SUPPORT_DST="$CONTAINER_LIB/Application Support"
PREFS_DST="$CONTAINER_LIB/Preferences"

echo "========================================================"
echo "  Devotion (還願) — Save Data & Preferences Restorer"
echo "========================================================"
echo ""

if [[ ! -d "$SAVE_SRC" ]]; then
    echo "❌ Error: Save folder not found at:"
    echo "   $SAVE_SRC"
    echo "   Make sure this script stays inside the same folder as your backup files."
    exit 1
fi

echo "📂 Restoring save files from:"
echo "   $SCRIPT_DIR"
echo ""

# Ensure target directories exist
mkdir -p "$APP_SUPPORT_DST"
mkdir -p "$PREFS_DST"

# Copy save data folder
rm -rf "$APP_SUPPORT_DST/inc.redcandlegames.devotion"
cp -R "$SAVE_SRC" "$APP_SUPPORT_DST/"
echo "✅ Restored save data to:"
echo "   $APP_SUPPORT_DST/inc.redcandlegames.devotion"

# Copy preferences plist if present
if [[ -f "$PLIST_SRC" ]]; then
    cp "$PLIST_SRC" "$PREFS_DST/inc.redcandlegames.devotion.plist"
    # Refresh macOS preferences daemon so restored PlayerPrefs take effect immediately
    killall cfprefsd 2>/dev/null || true
    echo "✅ Restored preferences (PlayerPrefs) to:"
    echo "   $PREFS_DST/inc.redcandlegames.devotion.plist"
fi

echo ""
echo "🎉 All Devotion save files have been restored successfully!"
echo "You can now close this window and launch Devotion."
