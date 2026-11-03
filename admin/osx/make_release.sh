#!/bin/bash
#
# Build a self-contained, Developer ID signed owncloud.online.app (+ .dmg) with
# the File Provider extension embedded. This is the macOS release path that
# works on recent SDKs where `craft --package` chokes on its own ad-hoc signing.
#
# It does NOT notarize; run admin/osx/notarize.sh afterwards (that needs the
# App Store Connect credentials, see NOTARIZATION.md). Pass --timestamp to sign
# with a secure timestamp (required before notarization); the default skips it
# for a fast local build. Extension build and signing are the same scripts the
# CI uses (make-fileprovider.sh, sign-app.sh).
#
# Env / defaults:
#   TEAM_ID        4545KLA52K (BW-TECH GmbH)
#   SIGN_IDENTITY  the "Developer ID Application" identity of TEAM_ID (name or SHA1); for a
#                  local test build e.g. "Apple Development: …" (no notarization possible then)
#   CRAFT_ROOT     ~/craft/CraftMaster/macos-clang-arm64
#   BASE_APP       $CRAFT_ROOT/Applications/KDE/owncloud.online.app  (craft install)
#   OUT            /tmp/ocrelease
set -euo pipefail

TEAM_ID="${TEAM_ID:-4545KLA52K}"
CRAFT_ROOT="${CRAFT_ROOT:-$HOME/craft/CraftMaster/macos-clang-arm64}"
BASE_APP="${BASE_APP:-$CRAFT_ROOT/Applications/KDE/owncloud.online.app}"
OUT="${OUT:-/tmp/ocrelease}"
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
TS="--timestamp=none"
[ "${1:-}" = "--timestamp" ] && TS="--timestamp"

DEVID="${SIGN_IDENTITY:-$(security find-identity -v -p codesigning | awk -v team="($TEAM_ID)" '/Developer ID Application/ && index($0, team) {print $2; exit}')}"
[ -n "$DEVID" ] || { echo "Kein 'Developer ID Application' Zertifikat für Team $TEAM_ID (siehe NOTARIZATION.md)"; exit 1; }
APP="$OUT/owncloud.online.app"; FW="$APP/Contents/Frameworks"
rm -rf "$OUT"; mkdir -p "$OUT"

echo "==> 1) Basis-App kopieren"
cp -R "$BASE_APP" "$APP"

echo "==> 2) File-Provider-Erweiterung unsigniert bauen + einbetten"
# eine Basis-App mit BUILD_MAC_FILE_PROVIDER bringt die Erweiterung schon mit: ersetzen, nicht hineinkopieren
APPEX="$(TEAM_ID="$TEAM_ID" "$REPO/admin/osx/make-fileprovider.sh" "$APP" "$OUT/fpbuild" | tail -1)"
mkdir -p "$APP/Contents/PlugIns"
rm -rf "$APP/Contents/PlugIns/FileProviderExt.appex"
ditto "$APPEX" "$APP/Contents/PlugIns/FileProviderExt.appex"

echo "==> 3) macdeployqt (Qt einbetten)"
"$CRAFT_ROOT/bin/macdeployqt" "$APP" -qmldir="$REPO/src" >/dev/null 2>&1 || true

echo "==> 4) Nachzügler: restliche craftroot-Abhängigkeiten/IDs auf @rpath umbiegen"
for pass in 1 2 3; do
  while IFS= read -r bin; do
    # LC_ID_DYLIB
    iddep="$(otool -D "$bin" 2>/dev/null | tail -n +2 | grep "^$CRAFT_ROOT/lib/" || true)"
    if [ -n "$iddep" ]; then chmod u+w "$bin"; install_name_tool -id "@rpath/$(basename "$iddep")" "$bin" 2>/dev/null || true; fi
    # LC_LOAD_DYLIB
    while IFS= read -r dep; do
      [ -z "$dep" ] && continue
      base="$(basename "$dep")"
      if [ ! -f "$FW/$base" ] && [ -f "$CRAFT_ROOT/lib/$base" ]; then
        cp "$CRAFT_ROOT/lib/$base" "$FW/$base"; chmod u+w "$FW/$base"
        install_name_tool -id "@rpath/$base" "$FW/$base" 2>/dev/null || true
      fi
      chmod u+w "$bin"; install_name_tool -change "$dep" "@rpath/$base" "$bin" 2>/dev/null || true
    done < <(otool -L "$bin" 2>/dev/null | tail -n +2 | awk '{print $1}' | grep "^$CRAFT_ROOT/lib/" || true)
  done < <(find "$APP/Contents" -type f \( -name "*.dylib" -o -name "*.so" \); echo "$APP/Contents/MacOS/owncloud.online")
done

echo "==> 5) Developer ID signieren (innen nach außen, Hardened Runtime, Entitlements je Erweiterung)"
SIGN_TIMESTAMP=0; [ "$TS" = "--timestamp" ] && SIGN_TIMESTAMP=1
SIGN_IDENTITY="$DEVID" TEAM_ID="$TEAM_ID" SIGN_TIMESTAMP="$SIGN_TIMESTAMP" "$REPO/admin/osx/sign-app.sh" "$APP"

echo "==> 6) Verifikation + DMG"
codesign --verify --deep --strict "$APP" && echo "   codesign --deep --strict OK"
DMG="$OUT/owncloud.online-arm64.dmg"; rm -f "$DMG"
# wie das CI-dmg: App plus Verknüpfung auf /Applications zum Hineinziehen
STAGE="$OUT/dmg-root"; rm -rf "$STAGE"; mkdir -p "$STAGE"
ditto "$APP" "$STAGE/$(basename "$APP")"; ln -s /Applications "$STAGE/Applications"
hdiutil create -volname "owncloud.online" -srcfolder "$STAGE" -ov -format UDZO "$DMG" >/dev/null
rm -rf "$STAGE"
codesign --force $TS --sign "$DEVID" "$DMG"
echo "==> FERTIG: $APP"
echo "           $DMG"
spctl -a -vvv --type exec "$APP" 2>&1 | head -2 || true
