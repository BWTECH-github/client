# macOS notarization (owncloud.online)

So that owncloud.online — including the File Provider extension — runs on other
users' Macs, the app must be signed with a **Developer ID Application**
certificate, hardened-runtime signed and **notarized** by Apple. Team:
`BW-TECH GmbH … (4545KLA52K)`.

`admin/osx/notarize.sh` does the signing + notarization + stapling. It needs two
one-time setup steps that can only be done with the account holder's Apple
account:

## 1. Developer ID Application certificate

Xcode → **Settings… (⌘,)** → **Accounts** → select the Apple Account → pick the
**BW-TECH GmbH** team → **Manage Certificates…** → **＋** (bottom-left) →
**Developer ID Application**. It appears in the keychain as
`Developer ID Application: … (4545KLA52K)`. Verify:

```
security find-identity -v -p codesigning | grep "Developer ID Application"
```

(Only the account holder can create this, and there is a small per-team limit —
reuse the existing one if it already exists.)

### Account gotchas (learned the hard way)

- **App-specific passwords fail** for the `bw.tech` domain account with
  `HTTP 401 … not allowed for primary authentication` — federated / managed
  Apple IDs (custom email domain) do not accept app-specific passwords. Use the
  App Store Connect API key below instead.
- The account must be **provisioned for App Store Connect**. Right after joining
  the Developer Program you may see *"Your Apple Account is not set up for
  iTunes Connect"* on appstoreconnect.apple.com — wait until Apple finishes
  provisioning (can take a few hours), and make sure you sign in with the Apple
  ID that actually holds the paid membership (team 4545KLA52K), not a different
  address.

### codesign keychain access

A freshly created "Developer ID Application" key is only authorised for Xcode,
so `codesign` from a script can fail with `errSecInternalComponent`. Run
`notarize.sh` from an interactive Terminal (approve the keychain prompt with
"Always Allow"), or pre-authorise codesign once:

```
security set-key-partition-list -S apple-tool:,apple:,codesign: -s \
  -k <login-keychain-password> ~/Library/Keychains/login.keychain-db
```

Note: a detached/background shell has no keychain access at all — always sign in
the foreground.

## 2. notarytool credential profile

Preferred: an **App Store Connect API key** (no password stored, works headless).
App Store Connect → **Users and Access → Integrations → App Store Connect API** →
generate a key with the **Developer** role → download `AuthKey_XXXXXX.p8` and note
the **Key ID** and **Issuer ID**. Then store it once:

```
xcrun notarytool store-credentials oc-notary \
  --key /pfad/AuthKey_XXXXXX.p8 --key-id <KEY_ID> --issuer <ISSUER_ID>
```

Alternative (Apple ID + app-specific password from appleid.apple.com):

```
xcrun notarytool store-credentials oc-notary \
  --apple-id dev@bw.tech --team-id 4545KLA52K --password <app-specific-password>
```

## 3. Run

```
admin/osx/notarize.sh /Applications/owncloud.online.app        # uses profile "oc-notary"
```

Signing is done by `admin/osx/sign-app.sh` (shared by `notarize.sh`,
`make_release.sh` and the CI): every Mach-O file deepest first, then the bundles
inner-to-outer, each extension with its **own** entitlements generated from the
source tree (`FileProviderExt.entitlements`, `FinderSyncExt.entitlements`: app
sandbox + app group `<TEAM_ID>.online.owncloud.desktopclient`), the app last with
`admin/osx/app.entitlements`. It then checks the seal, the team, the hardened
runtime and the entitlements of every bundle and fails on `get-task-allow`.
Never re-sign the finished app with a blanket `codesign --force --deep` or without
entitlements: that strips the extensions' sandbox/app group and pkd/fileproviderd
reject them.

The File Provider extension is built **unsigned** by
`admin/osx/make-fileprovider.sh` (bundle id, minimum macOS and architectures taken
from the app, app group from `TEAM_ID`), embedded, and only then everything is
signed - no Xcode account or provisioning profile is needed.

Local dry run of the release package (no notarization):

```
SIGN_IDENTITY=<SHA1 of the Developer ID> OUT=/tmp/ocrelease admin/osx/make_release.sh
APPLE_TEAM_ID=4545KLA52K SIGN_IDENTITY=<SHA1> admin/osx/ci-sign-notarize.sh --no-notarize <dir-with-dmg>
```

## CI (GitHub Actions)

The macOS job signs and notarizes the dmg on the runner when these repository
secrets are set (`.github/workflows/main.yml`, steps "Apple signing setup" and
"Sign and notarize macOS package", script `admin/osx/ci-sign-notarize.sh`):

| Secret | Content |
| --- | --- |
| `APPLE_CERT_P12` | Developer ID Application certificate with private key and the Developer ID G2 intermediate, `.p12`, base64 |
| `APPLE_CERT_PASSWORD` | password of the `.p12` |
| `APPLE_TEAM_ID` | `4545KLA52K` |
| `APPLE_API_KEY_P8` | App Store Connect API key (`AuthKey_<id>.p8`, text) for `notarytool` |
| `APPLE_API_KEY_ID` | key ID of that API key |
| `APPLE_API_ISSUER_ID` | issuer ID from App Store Connect → Users and Access → Integrations |

The team id must be the team of the certificate in `APPLE_CERT_P12`: it is the
prefix of the app group of app and extensions and, via
`SOCKETAPI_TEAM_IDENTIFIER_PREFIX` (set from `APPLE_TEAM_ID` in the "Prepare"
step), of the Finder extension's socket. The notarization API key must belong to
the same team.

Without the secrets the job builds an unsigned dmg without the File Provider
extension as before (it would not run without a team signature). With them the
script keeps the dmg layout: it converts the image to read-write, builds and embeds
`FileProviderExt.appex`, signs with `sign-app.sh`, renames the volume to
"owncloud.online", converts it back, signs the dmg, submits it to `notarytool` and
staples the ticket. `publish-release.yml` calls `macos-sign-release.yml` for the
published release as well; a dmg that is already signed by the team, notarized,
stapled and contains the extension is left unchanged there.
