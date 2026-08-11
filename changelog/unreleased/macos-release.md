Enhancement: macOS builds ship with the release (Apple Silicon)

The macOS CI job has been building and passing all tests for months; only
the packaging step failed - the craft blueprint derives the bundle name
from an environment variable with the fallback "owncloud", so it
assembled an empty owncloud.app while the real owncloud.online.app was
never dependency-bundled. The packaging step now exports the branded
name, and the release pipeline publishes the resulting
owncloud.online-client-x.y.z-macos-arm64.dmg with checksums.

The macOS client syncs in classic mode (full sync + selective sync);
virtual files ("online only" placeholders in Finder) require an Apple
FileProvider extension, which in turn requires an Apple Developer
certificate - see docs/mac-vfs-implementation-plan.md for the plan. The
app is ad-hoc signed, not notarized: on first launch macOS shows a
Gatekeeper warning (macOS 15+: System Settings -> Privacy & Security ->
"Open Anyway"; macOS 14: right-click -> Open).
