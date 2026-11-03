Enhancement: Signed and notarized macOS package (Apple Silicon)

The release ships owncloud.online-client-x.y.z-macos-arm64.dmg (macOS 13 or
newer, Apple Silicon) with checksums. The app and its Finder and File Provider
extensions are signed with the Developer ID of BW-TECH GmbH (hardened
runtime, each extension with its own sandbox and app group entitlements) and
the dmg is notarized by Apple, so it opens without a Gatekeeper warning.

The macOS CI job used to fail at packaging because the craft blueprint
derived the bundle name from an environment variable with the fallback
"owncloud"; the packaging step now exports the branded name. Signing no
longer re-signs the extensions without their entitlements, which had left
the Finder extension of the pre-releases without its sandbox.
