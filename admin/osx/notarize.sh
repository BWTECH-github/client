#!/bin/bash
#
# Deep-sign the built owncloud.online.app with a Developer ID identity + hardened
# runtime, then notarize and staple it so it runs on other Macs (Gatekeeper).
#
# Prerequisites (one-time, see admin/osx/NOTARIZATION.md):
#   1. A "Developer ID Application: … (4545KLA52K)" certificate in the keychain.
#   2. A stored notarytool credential profile:  xcrun notarytool store-credentials
#
# Usage:  admin/osx/notarize.sh /path/to/owncloud.online.app [notary-profile]
set -euo pipefail

APP="${1:?Pfad zur .app angeben}"
PROFILE="${2:-oc-notary}"
ENTITLEMENTS_DIR="$(cd "$(dirname "$0")" && pwd)"

IDENTITY="$(security find-identity -v -p codesigning | awk -F'"' '/Developer ID Application/{print $2; exit}')"
if [[ -z "${IDENTITY:-}" ]]; then
  echo "FEHLER: kein 'Developer ID Application' Zertifikat gefunden." >&2
  echo "Erst erstellen (Xcode > Settings > Accounts > Team > Manage Certificates > + > Developer ID Application)." >&2
  exit 1
fi
echo "Signiere mit: $IDENTITY"

sign() { # sign <entitlements-or-"-"> <path>
  local ent="$1" path="$2"
  if [[ "$ent" == "-" ]]; then
    codesign --force --options runtime --timestamp --sign "$IDENTITY" "$path"
  else
    codesign --force --options runtime --timestamp --entitlements "$ent" --sign "$IDENTITY" "$path"
  fi
}

# Inner code first: dylibs/so plugins, then frameworks, then the app-extensions,
# then the outer app. Each extension keeps its own entitlements.
find "$APP/Contents" \( -name "*.dylib" -o -name "*.so" \) -type f -print0 | while IFS= read -r -d '' f; do sign - "$f"; done
if [[ -d "$APP/Contents/Frameworks" ]]; then
  find "$APP/Contents/Frameworks" -maxdepth 1 -name "*.framework" -print0 | while IFS= read -r -d '' fw; do sign - "$fw"; done
fi
[[ -d "$APP/Contents/PlugIns/FinderSyncExt.appex" ]] && sign - "$APP/Contents/PlugIns/FinderSyncExt.appex"
# The File Provider extension is already signed with its own entitlements by
# xcodebuild; re-sign here with Developer ID + hardened runtime, keeping them.
FP_ENT="$(mktemp)"; codesign -d --entitlements "$FP_ENT" --xml "$APP/Contents/PlugIns/FileProviderExt.appex" 2>/dev/null || true
if [[ -s "$FP_ENT" && -d "$APP/Contents/PlugIns/FileProviderExt.appex" ]]; then sign "$FP_ENT" "$APP/Contents/PlugIns/FileProviderExt.appex"; fi

# Outer app: hardened runtime + the app group (must match the extensions').
sign "$ENTITLEMENTS_DIR/app.entitlements" "$APP"

echo "Prüfe Signatur…"
codesign --verify --deep --strict --verbose=2 "$APP"

ZIP="${APP%.app}.zip"
echo "Zippe → $ZIP"
/usr/bin/ditto -c -k --keepParent "$APP" "$ZIP"

echo "Reiche zur Notarisierung ein (Profil: $PROFILE)…"
xcrun notarytool submit "$ZIP" --keychain-profile "$PROFILE" --wait

echo "Stapeln…"
xcrun stapler staple "$APP"
spctl -a -vvv --type exec "$APP" || true
echo "FERTIG: $APP ist signiert, notarisiert und gestapelt."
