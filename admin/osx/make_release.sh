#!/bin/bash
#
# Build a self-contained, Developer ID signed owncloud.online.app (+ .dmg) with
# the File Provider extension embedded. This is the macOS release path that
# works on recent SDKs where `craft --package` chokes on its own ad-hoc signing.
#
# It does NOT notarize; run admin/osx/notarize.sh afterwards (that needs the
# App Store Connect credentials, see NOTARIZATION.md). Pass --timestamp to sign
# with a secure timestamp (required before notarization); the default skips it
# for a fast local build.
#
# Env / defaults:
#   TEAM_ID        4545KLA52K
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

DEVID="$(security find-identity -v -p codesigning | awk -F'"' '/Developer ID Application/{print $2; exit}')"
[ -n "$DEVID" ] || { echo "Kein 'Developer ID Application' Zertifikat (siehe NOTARIZATION.md)"; exit 1; }
APP="$OUT/owncloud.online.app"; FW="$APP/Contents/Frameworks"
rm -rf "$OUT"; mkdir -p "$OUT"

echo "==> 1) File-Provider-Extension bauen"
xcodebuild -project "$REPO/shell_integration/MacOSX/OwnCloudFileProvider/OwnCloudFileProvider.xcodeproj" \
  -target FileProviderExt -configuration Release "SYMROOT=$OUT/fpbuild" -allowProvisioningUpdates \
  "DEVELOPMENT_TEAM=$TEAM_ID" OC_APPLICATION_NAME=owncloud.online \
  OC_APPLICATION_REV_DOMAIN=online.owncloud.desktopclient "OC_SOCKETAPI_TEAM_IDENTIFIER_PREFIX=${TEAM_ID}." \
  >/dev/null

echo "==> 2) Basis-App kopieren + Appex einbetten"
cp -R "$BASE_APP" "$APP"
mkdir -p "$APP/Contents/PlugIns"
cp -R "$OUT/fpbuild/Release/FileProviderExt.appex" "$APP/Contents/PlugIns/FileProviderExt.appex"

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

echo "==> 5) Developer ID signieren (innen nach außen, Hardened Runtime)"
sign() { codesign --force --options runtime $TS "$@"; }
find "$FW" "$APP/Contents/PlugIns" -type f \( -name "*.dylib" -o -name "*.so" \) | while read -r f; do sign --sign "$DEVID" "$f"; done
find "$FW" -maxdepth 1 -name "*.framework" | while read -r fw; do sign --sign "$DEVID" "$fw"; done
for ax in FinderSyncExt FileProviderExt; do
  d="$APP/Contents/PlugIns/$ax.appex"; [ -d "$d" ] || continue
  ent="$OUT/$ax.entitlements"; codesign -d --entitlements "$ent" --xml "$d" 2>/dev/null || true
  if [ -s "$ent" ]; then sign --entitlements "$ent" --sign "$DEVID" "$d"; else sign --sign "$DEVID" "$d"; fi
done
sign --entitlements "$REPO/admin/osx/app.entitlements" --sign "$DEVID" "$APP"

echo "==> 6) Verifikation + DMG"
codesign --verify --deep --strict "$APP" && echo "   codesign --deep --strict OK"
DMG="$OUT/owncloud.online-arm64.dmg"; rm -f "$DMG"
hdiutil create -volname "owncloud.online" -srcfolder "$APP" -ov -format UDZO "$DMG" >/dev/null
echo "==> FERTIG: $APP"
echo "           $DMG"
spctl -a -vvv --type exec "$APP" 2>&1 | head -2 || true
