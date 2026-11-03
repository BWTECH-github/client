#!/bin/bash
#
# Baut die File-Provider-Erweiterung (FileProviderExt.appex) UNSIGNIERT passend
# zu einer fertigen owncloud.online.app: Bundle-ID, Mindest-macOS und
# Architekturen kommen aus der App, die App-Gruppe aus TEAM_ID. Signiert wird
# danach mit admin/osx/sign-app.sh (Developer ID, eigene Entitlements), so dass
# weder ein Apple-Konto in Xcode noch ein Provisioning-Profil nötig ist (CI).
#
# Aufruf:  TEAM_ID=4545KLA52K admin/osx/make-fileprovider.sh <App.app> <arbeitsordner>
# Ausgabe: Pfad der gebauten .appex auf stdout (letzte Zeile)
set -euo pipefail

APP="${1:?App angeben}"
WORK="${2:?Arbeitsordner angeben}"
: "${TEAM_ID:?TEAM_ID (Team der Developer ID) setzen}"
REPO="$(cd "$(dirname "$0")/../.." && pwd)"

info() { /usr/libexec/PlistBuddy -c "Print :$1" "$APP/Contents/Info.plist"; }
REV_DOMAIN="$(info CFBundleIdentifier)"
EXE="$(info CFBundleExecutable)"
APP_NAME="$(info CFBundleName 2>/dev/null || echo "$EXE")"
MIN_OS="$(info LSMinimumSystemVersion)"
ARCHS="$(lipo -archs "$APP/Contents/MacOS/$EXE")"
GROUP="$TEAM_ID.$REV_DOMAIN"

mkdir -p "$WORK"
LOG="$WORK/xcodebuild-fileprovider.log"
echo "File-Provider-Erweiterung: $REV_DOMAIN.FileProviderExt, Gruppe $GROUP, macOS $MIN_OS, $ARCHS" >&2
if ! xcodebuild -project "$REPO/shell_integration/MacOSX/OwnCloudFileProvider/OwnCloudFileProvider.xcodeproj" \
    -target FileProviderExt -configuration Release \
    "SYMROOT=$WORK/sym" "OBJROOT=$WORK/obj" \
    CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY= DEVELOPMENT_TEAM= \
    "ARCHS=$ARCHS" ONLY_ACTIVE_ARCH=NO "MACOSX_DEPLOYMENT_TARGET=$MIN_OS" \
    "OC_APPLICATION_NAME=$APP_NAME" "OC_APPLICATION_REV_DOMAIN=$REV_DOMAIN" \
    "OC_SOCKETAPI_TEAM_IDENTIFIER_PREFIX=$TEAM_ID." > "$LOG" 2>&1; then
  grep -E "error:|BUILD FAILED" "$LOG" | head -40 >&2 || tail -40 "$LOG" >&2
  exit 1
fi

APPEX="$WORK/sym/Release/FileProviderExt.appex"
got="$(/usr/libexec/PlistBuddy -c 'Print :NSExtension:NSExtensionFileProviderDocumentGroup' "$APPEX/Contents/Info.plist")"
if [[ "$got" != "$GROUP" ]]; then
  echo "FEHLER: NSExtensionFileProviderDocumentGroup ist '$got', erwartet '$GROUP'" >&2
  exit 1
fi
echo "$APPEX"
