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
HERE="$(cd "$(dirname "$0")" && pwd)"
TEAM_ID="${TEAM_ID:-4545KLA52K}"
IDENTITY="${SIGN_IDENTITY:-$(security find-identity -v -p codesigning | awk -v team="($TEAM_ID)" '/Developer ID Application/ && index($0, team) {print $2; exit}')}"
if [[ -z "${IDENTITY:-}" ]]; then
  echo "FEHLER: kein 'Developer ID Application' Zertifikat für Team $TEAM_ID gefunden." >&2
  echo "Erst erstellen (Xcode > Settings > Accounts > Team > Manage Certificates > + > Developer ID Application)." >&2
  exit 1
fi
echo "Signiere mit: $IDENTITY (Team $TEAM_ID)"
# Gleiche Signierlogik wie in der CI: innen nach außen, jede Erweiterung mit ihren
# eigenen Entitlements, App zuletzt, sicherer Zeitstempel
SIGN_IDENTITY="$IDENTITY" TEAM_ID="$TEAM_ID" SIGN_TIMESTAMP=1 "$HERE/sign-app.sh" "$APP"
ZIP="${APP%.app}.zip"
echo "Zippe → $ZIP"
/usr/bin/ditto -c -k --keepParent "$APP" "$ZIP"

echo "Reiche zur Notarisierung ein (Profil: $PROFILE)…"
xcrun notarytool submit "$ZIP" --keychain-profile "$PROFILE" --wait

echo "Stapeln…"
xcrun stapler staple "$APP"
spctl -a -vvv --type exec "$APP" || true
echo "FERTIG: $APP ist signiert, notarisiert und gestapelt."
