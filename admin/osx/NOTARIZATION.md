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

The script signs inner-to-outer (dylibs/plugins → frameworks → FinderSyncExt →
FileProviderExt with its own entitlements → the app with the shared app group),
verifies, zips, submits with `--wait`, staples, and checks with `spctl`. For the
Craft release build this replaces the ad-hoc signing; it can be wired into
`create_mac.sh` once the certificate + profile exist.
