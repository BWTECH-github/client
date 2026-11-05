#!/bin/bash
#
# CI: owncloud.online.app in jedem dmg eines Ordners fertigstellen: die
# File-Provider-Erweiterung bauen und einbetten, mit der Developer-ID-Identität
# signieren (admin/osx/sign-app.sh: Hardened Runtime, von innen nach außen,
# eigene Entitlements je Erweiterung, App zuletzt), das dmg neu packen,
# signieren, bei Apple notarisieren und das Ticket anheften. Das Layout des dmg
# bleibt erhalten, weil das Abbild nur beschreibbar umgewandelt und nicht neu
# gebaut wird.
#
# Ein dmg, das schon mit diesem Team signiert, notarisiert und angeheftet ist und
# die Erweiterung enthält, bleibt unverändert (publish-release.yml ruft
# macos-sign-release.yml auch für Pakete auf, die main.yml schon signiert hat).
#
# Erwartet (setzt der Schritt "Apple signing setup" in den Workflows):
#   APPLE_TEAM_ID        Team-ID der Identität (Präfix der App-Gruppe)
#   SIGN_KEYCHAIN        Schlüsselbund mit der Developer-ID-Identität (optional,
#                        ohne: Standard-Suchliste)
#   SIGN_IDENTITY        optional, SHA1 der Identität (sonst die Developer ID des Teams)
#   APPLE_API_KEY_PATH   App-Store-Connect-API-Schlüssel (.p8) für notarytool
#   APPLE_API_KEY_ID, APPLE_API_ISSUER_ID
#   EMBED_FILE_PROVIDER  1 (Vorgabe): Erweiterung bauen und einbetten; 0: nicht
#
# Aufruf:  admin/osx/ci-sign-notarize.sh [--no-notarize] <ordner-mit-dmg>
#          --no-notarize: nur einbetten und signieren (lokaler Probelauf)
set -euo pipefail

NOTARIZE=1
if [[ "${1:-}" == "--no-notarize" ]]; then NOTARIZE=0; shift; fi
DIR="${1:?Ordner mit dem dmg angeben}"
: "${APPLE_TEAM_ID:?}"
if [[ "$NOTARIZE" == 1 ]]; then
  : "${APPLE_API_KEY_PATH:?}" "${APPLE_API_KEY_ID:?}" "${APPLE_API_ISSUER_ID:?}"
fi
HERE="$(cd "$(dirname "$0")" && pwd)"
KC=()
[[ -n "${SIGN_KEYCHAIN:-}" ]] && KC=("$SIGN_KEYCHAIN")

IDENTITY="${SIGN_IDENTITY:-$(security find-identity -v -p codesigning ${KC[@]+"${KC[@]}"} | awk -v team="($APPLE_TEAM_ID)" '/Developer ID Application/ && index($0, team) {print $2; exit}')}"
if [[ -z "$IDENTITY" ]]; then
  echo "FEHLER: keine Developer-ID-Identität für Team $APPLE_TEAM_ID im Schlüsselbund" >&2
  exit 1
fi

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

# schon fertig: Team stimmt, Erweiterung mit App-Gruppe drin, notarisiert und angeheftet
already_done() {
  local dmg="$1" mnt app ok=1
  xcrun stapler validate -q "$dmg" >/dev/null 2>&1 || return 1
  spctl -a -t open --context context:primary-signature "$dmg" >/dev/null 2>&1 || return 1
  mnt="$(mktemp -d)"
  hdiutil attach "$dmg" -readonly -nobrowse -noverify -noautoopen -mountpoint "$mnt" -quiet || return 1
  app="$(find "$mnt" -maxdepth 1 -name '*.app' -print -quit)"
  if [[ -z "$app" ]] ||
     [[ "$(codesign -dv "$app" 2>&1 | sed -n 's/^TeamIdentifier=//p')" != "$APPLE_TEAM_ID" ]] ||
     ! codesign -d --entitlements - --xml "$app/Contents/PlugIns/FileProviderExt.appex" 2>/dev/null | grep -q "$APPLE_TEAM_ID\."; then
    ok=0
  fi
  hdiutil detach "$mnt" -quiet || true
  rmdir "$mnt" 2>/dev/null || true
  [[ "$ok" == 1 ]]
}

shopt -s nullglob
found=0
for DMG in "$DIR"/*.dmg; do
  found=1
  echo "== $(basename "$DMG")"
  if [[ "$NOTARIZE" == 1 ]] && already_done "$DMG"; then
    echo "bereits mit Team $APPLE_TEAM_ID signiert, notarisiert und mit Erweiterung – unverändert"
    continue
  fi
  WORK="$(mktemp -d)"
  RW="$WORK/rw.dmg"
  MNT="$WORK/mnt"
  mkdir -p "$MNT"

  # beschreibbare Kopie mit Platz für Erweiterung und Signaturen
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

  # Erweiterung passend zur App bauen und einbetten, bevor irgendetwas signiert wird
  if [[ "${EMBED_FILE_PROVIDER:-1}" == "1" ]]; then
    APPEX="$(TEAM_ID="$APPLE_TEAM_ID" "$HERE/make-fileprovider.sh" "$APP" "$WORK/fileprovider" | tail -1)"
    mkdir -p "$APP/Contents/PlugIns"
    rm -rf "$APP/Contents/PlugIns/FileProviderExt.appex"
    ditto "$APPEX" "$APP/Contents/PlugIns/FileProviderExt.appex"
    echo "FileProviderExt.appex eingebettet"
  fi

  SIGN_IDENTITY="$IDENTITY" TEAM_ID="$APPLE_TEAM_ID" SIGN_KEYCHAIN="${SIGN_KEYCHAIN:-}" \
    SIGN_TIMESTAMP="$NOTARIZE" "$HERE/sign-app.sh" "$APP"
  spctl -a -vvv --type exec "$APP" || true

  # sichtbarer Name des Laufwerks statt des craft-Paketnamens
  diskutil rename "$MNT" "owncloud.online" >/dev/null 2>&1 || echo "WARNUNG: Laufwerk nicht umbenannt"

  hdiutil detach "$MNT" -quiet
  OUT="$WORK/$(basename "$DMG")"
  hdiutil convert "$RW" -format UDZO -imagekey zlib-level=9 -o "$OUT" -quiet
  if [[ "$NOTARIZE" == 1 ]]; then
    codesign --force --timestamp ${SIGN_KEYCHAIN:+--keychain "$SIGN_KEYCHAIN"} --sign "$IDENTITY" "$OUT"
    notarize "$OUT" "$WORK/notary.json"
    # Das Ticket holt stapler über das Netz von Apple; kurze Aussetzer wiederholen
    for try in 1 2 3 4 5 6; do
      if xcrun stapler staple "$OUT"; then break; fi
      [[ "$try" -eq 6 ]] && exit 1
      sleep $(( try * 20 ))
    done
    # Auch die Prüfung fragt Apple über das Netz, derselbe Aussetzer darf den Lauf nicht beenden
    for try in 1 2 3 4 5 6; do
      if xcrun stapler validate "$OUT"; then break; fi
      [[ "$try" -eq 6 ]] && exit 1
      sleep $(( try * 20 ))
    done
    spctl -a -vvv -t open --context context:primary-signature "$OUT"
  else
    codesign --force --timestamp=none ${SIGN_KEYCHAIN:+--keychain "$SIGN_KEYCHAIN"} --sign "$IDENTITY" "$OUT"
  fi

  mv -f "$OUT" "$DMG"
  if [[ -f "$DMG.sha256" ]]; then
    (cd "$DIR" && shasum -a 256 "$(basename "$DMG")" > "$(basename "$DMG").sha256")
  fi
  rm -rf "$WORK"
  if [[ "$NOTARIZE" == 1 ]]; then
    echo "fertig: $(basename "$DMG") mit Erweiterung, signiert, notarisiert, angeheftet"
  else
    echo "fertig: $(basename "$DMG") mit Erweiterung, signiert (nicht notarisiert)"
  fi
done

if [[ "$found" -eq 0 ]]; then
  echo "FEHLER: kein dmg in $DIR" >&2
  exit 1
fi
