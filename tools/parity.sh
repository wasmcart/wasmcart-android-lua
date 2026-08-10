#!/usr/bin/env bash
# The parity gate: one command. Run it at every wasmcart-lua bump.
#
# Builds a wasm engine and a native engine from the SAME sources with the
# SAME feature set, packs each game against the wasm one, then runs both
# through identical scripted input and compares the frames.
set -euo pipefail
cd "$(dirname "$0")/.."
HERE="$(pwd)"
GAMES="${GAMES_REPO:-$HERE/../games-for-dad}"
WORK="$HERE/build-parity/carts"

echo "== building native engine"
WITH_PHYSICS=${WITH_PHYSICS:-0} ./tools/build-native-macos.sh >/dev/null

echo "== building matched wasm engine"
./tools/build-parity-wasm.sh >/dev/null

echo "== packing carts against the matched engine"
mkdir -p "$WORK"
for spec in "jacksorbetter:Jacks or Better" "fivecardstud:5 Card Stud" \
            "sevencardstud:7 Card Stud" "spades:Spades"; do
    g="${spec%%:*}"; n="${spec##*:}"
    [ -d "$GAMES/$g/app" ] || { echo "  skip $g (not found)"; continue; }
    ( cd "$GAMES/$g" && npx wasmcart pack \
        --wasm "$HERE/build-parity/engine-parity.wasm" --assets app \
        --name "$n" --width 1920 --height 1080 -o "$WORK/$g.wasc" >/dev/null )
done

echo "== comparing"
node tools/parity.mjs --frames "${FRAMES:-300}" --seed "${SEED:-12345}" \
     --drive "${DRIVE:-90}" "$WORK"/*.wasc
