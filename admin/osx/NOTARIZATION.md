# macOS notarization (owncloud.online)

So that owncloud.online — including the File Provider extension — runs on other
users' Macs, the app must be signed with a **Developer ID Application**
certificate, hardened-runtime signed and **notarized** by Apple. Team:
`BW-TECH GmbH … (A7VZ8HFNR4)`.

`admin/osx/notarize.sh` does the signing + notarization + stapling. It needs two
one-time setup steps that can only be done with the account holder's Apple
account:

## 1. Developer ID Application certificate

Xcode → **Settings… (⌘,)** → **Accounts** → select the Apple Account → pick the
**BW-TECH GmbH** team → **Manage Certificates…** → **＋** (bottom-left) →
**Developer ID Application**. It appears in the keychain as
`Developer ID Application: … (A7VZ8HFNR4)`. Verify:

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
  ID that actually holds the paid membership (team A7VZ8HFNR4), not a different
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
  --apple-id dev@bw.tech --team-id A7VZ8HFNR4 --password <app-specific-password>
```

## 3. Run

```
admin/osx/notarize.sh /Applications/owncloud.online.app        # uses profile "oc-notary"
```

The script signs inner-to-outer (dylibs/plugins → frameworks → FinderSyncExt →
FileProviderExt with its own entitlements → the app with the shared app group),
verifies, zips, submits with `--wait`, staples, and checks with `spctl`. For the
Craft release build this replaces the ad-hoc signing; it can be wired into
`create_mac.sh` once the certificate + profile exist.

## CI (GitHub Actions)

The macOS job signs and notarizes the dmg on the runner when these repository
secrets are set (`.github/workflows/main.yml`, steps "Apple signing setup" and
"Sign and notarize macOS package", script `admin/osx/ci-sign-notarize.sh`):

| Secret | Content |
| --- | --- |
| `APPLE_CERT_P12` | Developer ID Application certificate with private key and the Developer ID G2 intermediate, `.p12`, base64 |
| `APPLE_CERT_PASSWORD` | password of the `.p12` |
| `APPLE_TEAM_ID` | `A7VZ8HFNR4` |
| `APPLE_API_KEY_P8` | App Store Connect API key (`AuthKey_<id>.p8`, text) for `notarytool` |
| `APPLE_API_KEY_ID` | key ID of that API key |
| `APPLE_API_ISSUER_ID` | issuer ID from App Store Connect → Users and Access → Integrations |

Without the secrets the job builds an unsigned dmg as before. The script keeps the
dmg layout: it converts the image to read-write, signs everything inside the app
from the inside out with the hardened runtime, converts it back, signs the dmg,
submits it to `notarytool` and staples the ticket.
