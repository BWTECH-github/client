#!/bin/bash
#
# CI: owncloud.online.app in jedem dmg eines Ordners mit der Developer-ID-Identität
# signieren (Hardened Runtime, von innen nach außen), das dmg neu packen, signieren,
# bei Apple notarisieren und das Ticket anheften. Das Layout des dmg bleibt erhalten,
# weil das Abbild nur beschreibbar umgewandelt und nicht neu gebaut wird.
#
# Erwartet (setzt der Schritt "Apple signing setup" in .github/workflows/main.yml):
#   SIGN_KEYCHAIN        Schlüsselbund mit der Developer-ID-Identität
#   APPLE_TEAM_ID        Team-ID der Identität
#   APPLE_API_KEY_PATH   App-Store-Connect-API-Schlüssel (.p8) für notarytool
#   APPLE_API_KEY_ID, APPLE_API_ISSUER_ID
#
# Aufruf:  admin/osx/ci-sign-notarize.sh <ordner-mit-dmg>
set -euo pipefail

DIR="${1:?Ordner mit dem dmg angeben}"
: "${SIGN_KEYCHAIN:?}" "${APPLE_TEAM_ID:?}" "${APPLE_API_KEY_PATH:?}" "${APPLE_API_KEY_ID:?}" "${APPLE_API_ISSUER_ID:?}"

IDENTITY="$(security find-identity -v -p codesigning "$SIGN_KEYCHAIN" | awk -v team="($APPLE_TEAM_ID)" '/Developer ID Application/ && index($0, team) {print $2; exit}')"
if [[ -z "$IDENTITY" ]]; then
  echo "FEHLER: keine Developer-ID-Identität für Team $APPLE_TEAM_ID im Schlüsselbund" >&2
  exit 1
fi

sign() {
  codesign --force --options runtime --timestamp --keychain "$SIGN_KEYCHAIN" --sign "$IDENTITY" "$1"
}

notarize() {
  local file="$1" out="$2"
  xcrun notarytool submit "$file" --key "$APPLE_API_KEY_PATH" --key-id "$APPLE_API_KEY_ID" \
    --issuer "$APPLE_API_ISSUER_ID" --wait --timeout 45m --output-format json > "$out"
  local status id
  status="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("status", ""))' "$out")"
  id="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("id", ""))' "$out")"
  echo "Notarisierung $id: $status"
  if [[ "$status" != "Accepted" ]]; then
    [[ -n "$id" ]] && xcrun notarytool log "$id" --key "$APPLE_API_KEY_PATH" \
      --key-id "$APPLE_API_KEY_ID" --issuer "$APPLE_API_ISSUER_ID" || true
    return 1
  fi
}

shopt -s nullglob
found=0
for DMG in "$DIR"/*.dmg; do
  found=1
  echo "== $(basename "$DMG")"
  WORK="$(mktemp -d)"
  RW="$WORK/rw.dmg"
  MNT="$WORK/mnt"
  mkdir -p "$MNT"

  # beschreibbare Kopie mit Platz für die Signaturen
  hdiutil convert "$DMG" -format UDRW -o "$RW" -quiet
  size_mb=$(( $(stat -f %z "$RW") / 1048576 + 200 ))
  hdiutil resize -size "${size_mb}m" "$RW"
  hdiutil attach "$RW" -nobrowse -noverify -noautoopen -mountpoint "$MNT" -quiet

  APP="$(find "$MNT" -maxdepth 1 -name '*.app' -print -quit)"
  if [[ -z "$APP" ]]; then
    echo "FEHLER: keine .app im dmg" >&2
    hdiutil detach "$MNT" -quiet || true
    exit 1
  fi

  # 1. alle Mach-O-Dateien, tiefste zuerst
  find "$APP" -type f -print0 | while IFS= read -r -d '' f; do
    if file -b "$f" | grep -q 'Mach-O'; then
      depth="${f//[^\/]/}"
      printf '%d\t%s\n' "${#depth}" "$f"
    fi
  done | sort -rn | cut -f2- | while IFS= read -r f; do sign "$f"; done

  # 2. Bündel innerhalb der App, innere zuerst (-depth)
  find "$APP/Contents" -depth -type d \( -name '*.framework' -o -name '*.app' -o -name '*.xpc' -o -name '*.appex' \) -print0 |
    while IFS= read -r -d '' b; do sign "$b"; done

  # 3. die App selbst
  sign "$APP"
  codesign --verify --deep --strict --verbose=2 "$APP"
  spctl -a -vvv --type exec "$APP" || true

  hdiutil detach "$MNT" -quiet
  OUT="$WORK/$(basename "$DMG")"
  hdiutil convert "$RW" -format UDZO -imagekey zlib-level=9 -o "$OUT" -quiet
  codesign --force --timestamp --keychain "$SIGN_KEYCHAIN" --sign "$IDENTITY" "$OUT"

  notarize "$OUT" "$WORK/notary.json"
  xcrun stapler staple "$OUT"
  xcrun stapler validate "$OUT"
  spctl -a -vvv -t open --context context:primary-signature "$OUT"

  mv -f "$OUT" "$DMG"
  if [[ -f "$DMG.sha256" ]]; then
    (cd "$DIR" && shasum -a 256 "$(basename "$DMG")" > "$(basename "$DMG").sha256")
  fi
  rm -rf "$WORK"
  echo "fertig: $(basename "$DMG") signiert, notarisiert, angeheftet"
done

if [[ "$found" -eq 0 ]]; then
  echo "FEHLER: kein dmg in $DIR" >&2
  exit 1
fi
