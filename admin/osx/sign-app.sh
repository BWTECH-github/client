#!/bin/bash
#
# Signiert eine fertig zusammengestellte owncloud.online.app mit der Developer ID
# (Hardened Runtime), von innen nach außen, die App zuletzt. Jede Erweiterung
# bekommt ihre eigenen Entitlements aus dem Quellbaum (App-Sandbox, App-Gruppe
# <TEAM_ID>.<Bundle-ID>), die App admin/osx/app.entitlements. Ein pauschales
# Neusignieren ohne Entitlements würde sie den Erweiterungen nehmen: pkd lehnt
# die Finder-Erweiterung dann ab ("plug-ins must be sandboxed"), die
# File-Provider-Erweiterung startet nicht.
#
# Erst signieren, wenn alles eingebettet ist (FileProviderExt.appex, macdeployqt):
# jede spätere Änderung bricht das Siegel der App.
#
# Aufruf:  admin/osx/sign-app.sh <App.app>
# Umgebung:
#   SIGN_IDENTITY   Developer-ID-Identität (SHA1 oder Name), Pflicht
#   TEAM_ID         Team der Identität, Pflicht (Präfix der App-Gruppe)
#   SIGN_KEYCHAIN   Schlüsselbund mit der Identität (optional, CI)
#   SIGN_TIMESTAMP  1 (Vorgabe): sicherer Zeitstempel, nötig für die Notarisierung;
#                   0: ohne, für schnelle lokale Probebauten
set -euo pipefail

APP="${1:?App angeben}"
: "${SIGN_IDENTITY:?}" "${TEAM_ID:?}"
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
ENT_DIR="$(mktemp -d)"
trap 'rm -rf "$ENT_DIR"' EXIT

TS=(--timestamp)
[[ "${SIGN_TIMESTAMP:-1}" == "0" ]] && TS=(--timestamp=none)
KC=()
[[ -n "${SIGN_KEYCHAIN:-}" ]] && KC=(--keychain "$SIGN_KEYCHAIN")

sign() { codesign --force --options runtime "${TS[@]}" ${KC[@]+"${KC[@]}"} --sign "$SIGN_IDENTITY" "$@"; }
plist_get() { /usr/libexec/PlistBuddy -c "Print :$2" "$1" 2>/dev/null || true; }

REV_DOMAIN="$(plist_get "$APP/Contents/Info.plist" CFBundleIdentifier)"
APP_GROUP="$TEAM_ID.$REV_DOMAIN"

# Entitlements aus einer Vorlage im Quellbaum mit der App-Gruppe $2 nach $3
make_entitlements() {
  local template="$1" group="$2" out="$3"
  sed -E -e "s#[\$][(]OC_SOCKETAPI_TEAM_IDENTIFIER_PREFIX[)][\$][(]OC_APPLICATION_REV_DOMAIN[)]#$group#" \
      -e "s#<string>[A-Z0-9]{10}\.[^<]+</string>#<string>$group</string>#" "$template" > "$out"
  plutil -lint -s "$out"
  if grep -q '\$(' "$out"; then
    echo "FEHLER: nicht ersetzte Variable in $out" >&2
    exit 1
  fi
}

# Entitlements einer Erweiterung: bekannte aus dem Quellbaum, sonst die vorhandenen behalten
appex_entitlements() {
  local appex="$1" name out group
  name="$(basename "$appex" .appex)"
  out="$ENT_DIR/$name.entitlements"
  case "$name" in
    FileProviderExt)
      group="$(plist_get "$appex/Contents/Info.plist" NSExtension:NSExtensionFileProviderDocumentGroup)"
      if [[ "$group" != "$APP_GROUP" ]]; then
        echo "FEHLER: $name gehört zur App-Gruppe '$group', die App zu '$APP_GROUP' (Team der Signatur: $TEAM_ID)" >&2
        exit 1
      fi
      make_entitlements "$REPO/shell_integration/MacOSX/OwnCloudFileProvider/FileProviderExt/FileProviderExt.entitlements" "$group" "$out"
      ;;
    FinderSyncExt)
      # Gruppe = Präfix des Mach-Ports der Socket-API (SocketApiPrefix), sonst findet die
      # Erweiterung den Client aus der Sandbox heraus nicht
      group="$(plist_get "$appex/Contents/Info.plist" SocketApiPrefix)"
      if [[ "$group" != "$TEAM_ID."* ]]; then
        echo "WARNUNG: $name mit App-Gruppe '$group' ohne Team-Präfix $TEAM_ID. gebaut (SOCKETAPI_TEAM_IDENTIFIER_PREFIX)" >&2
      fi
      make_entitlements "$REPO/shell_integration/MacOSX/OwnCloudFinderSync/FinderSyncExt/FinderSyncExt.entitlements" "$group" "$out"
      ;;
    *)
      codesign -d --entitlements - --xml "$appex" > "$out" 2>/dev/null || true
      ;;
  esac
  echo "$out"
}

echo "== Signieren: $APP (Team $TEAM_ID, App-Gruppe $APP_GROUP)"

# 1. alle Mach-O-Dateien, tiefste zuerst (Bündel-Programme bekommen ihre
#    Entitlements in Schritt 2/3 mit dem Bündel)
find "$APP" -type f -print0 | while IFS= read -r -d '' f; do
  if file -b "$f" | grep -q 'Mach-O'; then
    depth="${f//[^\/]/}"
    printf '%d\t%s\n' "${#depth}" "$f"
  fi
done | sort -rn | cut -f2- | while IFS= read -r f; do sign "$f"; done

# 2. Bündel innerhalb der App, innere zuerst (-depth); Erweiterungen mit ihren Entitlements
find "$APP/Contents" -depth -type d \( -name '*.framework' -o -name '*.app' -o -name '*.xpc' -o -name '*.appex' \) -print0 |
  while IFS= read -r -d '' b; do
    if [[ "$b" == *.appex ]]; then
      ent="$(appex_entitlements "$b")"
      if [[ -s "$ent" ]]; then sign --entitlements "$ent" "$b"; else sign "$b"; fi
    else
      sign "$b"
    fi
  done

# 3. die App selbst, zuletzt
make_entitlements "$REPO/admin/osx/app.entitlements" "$APP_GROUP" "$ENT_DIR/app.entitlements"
sign --entitlements "$ENT_DIR/app.entitlements" "$APP"

# Prüfen: Siegel, Team, Entitlements je Bündel
codesign --verify --deep --strict --verbose=1 "$APP"
check_entitlements() {
  local bundle="$1" group="$2" sandbox="$3" got
  got="$ENT_DIR/check.plist"
  codesign -d --entitlements - --xml "$bundle" > "$got" 2>/dev/null
  if ! /usr/libexec/PlistBuddy -c "Print :com.apple.security.application-groups" "$got" 2>/dev/null | grep -qxF "    $group"; then
    echo "FEHLER: $(basename "$bundle") ohne App-Gruppe $group" >&2
    exit 1
  fi
  if [[ "$sandbox" == "1" ]] && [[ "$(plist_get "$got" com.apple.security.app-sandbox)" != "true" ]]; then
    echo "FEHLER: $(basename "$bundle") ohne App-Sandbox" >&2
    exit 1
  fi
  if [[ "$(plist_get "$got" com.apple.security.get-task-allow)" == "true" ]]; then
    echo "FEHLER: $(basename "$bundle") mit get-task-allow (Notarisierung lehnt ab)" >&2
    exit 1
  fi
  local info team
  info="$(codesign -dv "$bundle" 2>&1)"
  team="$(sed -n 's/^TeamIdentifier=//p' <<< "$info")"
  if [[ "$team" != "$TEAM_ID" ]]; then
    echo "FEHLER: $(basename "$bundle") mit Team '$team' signiert, erwartet $TEAM_ID" >&2
    exit 1
  fi
  if ! grep -q 'flags=.*(runtime)' <<< "$info"; then
    echo "FEHLER: $(basename "$bundle") ohne Hardened Runtime" >&2
    exit 1
  fi
  echo "   ok: $(basename "$bundle") (Gruppe $group$([[ "$sandbox" == 1 ]] && echo ', Sandbox'), Hardened Runtime)"
}
check_entitlements "$APP" "$APP_GROUP" 0
FP="$APP/Contents/PlugIns/FileProviderExt.appex"
[[ -d "$FP" ]] && check_entitlements "$FP" "$APP_GROUP" 1
FS="$APP/Contents/PlugIns/FinderSyncExt.appex"
[[ -d "$FS" ]] && check_entitlements "$FS" "$(plist_get "$FS/Contents/Info.plist" SocketApiPrefix)" 1
echo "== signiert: $APP"
