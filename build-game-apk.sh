#!/usr/bin/env bash
# Build a per-game APK from a .wasc cart.
#
#   ./build-game-apk.sh path/to/game.wasc [icon.png] [out.apk]
#
# The cart IS the app: gradle reads the cart's manifest and derives the
# launcher label ("7 Card Stud") and a unique applicationId
# (dev.wasmcart.player.g7cardstud), so every game installs ALONGSIDE the
# others rather than over them.
#
# icon.png is a 1024x1024 adaptive-icon FOREGROUND (transparent background,
# content within the center ~61% safe zone - launchers mask the rest). If
# omitted, an icon.png sitting next to the cart is used; failing that, the
# repo's default card-fan icon ships. Requires ImageMagick when an icon is
# given.
#
# Output lands at out/<slug>.apk unless a third argument names it.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
CART="${1:?usage: build-game-apk.sh cart.wasc [icon.png] [out.apk]}"
ICON="${2:-}"

[ -f "$CART" ] || { echo "no such cart: $CART" >&2; exit 1; }

# default icon: one sitting beside the cart
if [ -z "$ICON" ] && [ -f "$(dirname "$CART")/icon.png" ]; then
    ICON="$(dirname "$CART")/icon.png"
fi

# JAVA_HOME: fall back to Android Studio's bundled JDK
if [ -z "${JAVA_HOME:-}" ] && [ -d "/Applications/Android Studio.app/Contents/jbr/Contents/Home" ]; then
    export JAVA_HOME="/Applications/Android Studio.app/Contents/jbr/Contents/Home"
fi

cp "$CART" "$HERE/app/src/main/assets/cart.wasc"

if [ -n "$ICON" ]; then
    [ -f "$ICON" ] || { echo "no such icon: $ICON" >&2; exit 1; }
    for spec in mdpi:108 hdpi:162 xhdpi:216 xxhdpi:324 xxxhdpi:432; do
        d="${spec%%:*}"; px="${spec##*:}"
        mkdir -p "$HERE/app/src/main/res/mipmap-$d"
        magick "$ICON" -resize "${px}x${px}" \
            "$HERE/app/src/main/res/mipmap-$d/ic_launcher_foreground.png"
    done
fi

# optional per-game icon background: icon-bg.txt next to the cart holds
# a #RRGGBB line (default: the family felt green)
GRADLE_ARGS=()
# WC_ABI overrides the native ABI, for running on an EMULATOR (x86_64 on a
# desktop) rather than a phone. Ships as arm64 when unset.
if [ -n "${WC_ABI:-}" ]; then
    GRADLE_ARGS+=("-PwcAbi=$WC_ABI")
fi
# WC_ENGINE_REPO builds against a wasmcart-lua WORKING TREE rather than the
# pinned submodule -- what you want while changing the engine and a cart
# together, since otherwise the APK ships the pinned engine and the fix
# under test never reaches the device.
if [ -n "${WC_ENGINE_REPO:-}" ]; then
    GRADLE_ARGS+=("-PengineRepo=$WC_ENGINE_REPO")
fi
# WC_PHYSICS=ON builds Box2D + Box3D in. A cart that calls b2/b3 gets
# nothing at all without them, and they are the biggest thing in the
# binary, so this is opt-in rather than always-on.
if [ -n "${WC_PHYSICS:-}" ]; then
    GRADLE_ARGS+=("-PwithPhysics=$WC_PHYSICS")
fi
if [ -f "$(dirname "$CART")/icon-bg.txt" ]; then
    GRADLE_ARGS+=("-PiconBg=$(head -1 "$(dirname "$CART")/icon-bg.txt" | tr -d '[:space:]')")
fi

(cd "$HERE" && ./gradlew assembleDebug -q "${GRADLE_ARGS[@]+"${GRADLE_ARGS[@]}"}")

NAME=$(unzip -p "$CART" manifest.json | python3 -c 'import sys,json; print(json.load(sys.stdin).get("name","game"))')
SLUG=$(printf '%s' "$NAME" | tr '[:upper:]' '[:lower:]' | tr -cd 'a-z0-9')
OUT="${3:-$HERE/out/${SLUG:-game}.apk}"
mkdir -p "$(dirname "$OUT")"
cp "$HERE/app/build/outputs/apk/debug/app-debug.apk" "$OUT"
echo "built: $OUT"
echo "label: $NAME"
