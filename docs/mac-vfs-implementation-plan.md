# macOS Virtual Files (FileProvider) — Implementation Plan / Handoff

Goal: give the owncloud.online desktop client the same "OneDrive-like" virtual-files
experience on macOS that it already has on Windows (Cloud Files API). One codebase,
one client — the Windows build keeps `vfs/win` (CfApi), the macOS build adds a new
`vfs/mac` backed by Apple's **FileProvider** framework. No separate Mac fork.

## Status (November 2026)

Implemented; sections 0–6 below are the original plan and kept for reference.

- **Extension** (`shell_integration/MacOSX/OwnCloudFileProvider/FileProviderExt`): an
  `NSFileProviderReplicatedExtension` that talks WebDAV itself (`WebDavBackend.swift`).
  One domain = one folder of an account. Settings come from
  `FileProviderConfig-<domain>.json` in the app group container (no secrets, re-read when
  the client rewrites them, e.g. after a server move); the `Authorization` header comes
  from the login keychain (service `<app group>.fileprovider`, account = domain id, access
  list: client + extension). The data protection keychain is not an option: without a
  provisioning profile it refuses app groups as access groups (`-34018`). After a 401 the
  extension posts the Darwin notification `<app group>.fileprovider.credentials` and waits
  up to 12 s for a new header; it never refreshes tokens itself.
- **Change detection**: the client polls the folder ETag every `remotePollInterval` and
  calls `signalEnumerator(.workingSet)`; the extension walks from the root into known
  folders whose ETag changed, tells moves from deletions by file id (`dav/meta`), and
  stages each batch until the system acknowledges its anchor (`ItemStore`, two-phase).
- **Data safety**: uploads with `If-Match` (new files `If-None-Match: *`), moves with
  `Overwrite: F`, file deletes with `If-Match`, recursive folder deletes/trash only if the
  folder ETag is unchanged (else `deletionRejected` / handed back), conflicting edits kept
  as `name (conflicted copy <date>).ext`, SHA1 checked on download (`OC-Checksum`) and sent
  on upload. Trash = delete on the server (owncloud.online keeps it in its trash bin).
- **App side** (`src/plugins/vfs/mac`): domain per folder (id = SHA1 of account uuid + local
  path; display name = `user@host`, plus the folder name for a sub folder or space; the Finder
  shows it as "owncloud.online - user@host"), credential mirroring, polling, sign-out → disconnect + drop credentials, pause →
  temporary disconnect, folder/account removal → domain + settings + keychain item removed,
  stale domains (no settings file) removed at start, System Settings switch off (`FP -2011`)
  shown as folder error plus a one-time hint with "Open System Settings".
- **Keep / free space**: "Always keep on this device", "Free up space (online only)" and "Reset
  to default" (client folder view) end in `VfsMac::setPinState`: besides the pin database it
  writes one request file `FileProviderPin-<domain>-<time>-<uuid>.json` (`{path, state}`, state
  `local`/`online`/`inherited`) to the app group container and signals the working set. The
  extension takes the requests in `enumerateChanges`, keeps the choice per item id in its store
  (a choice replaces those of everything below), reports the item with the new `contentPolicy`
  (`downloadEagerlyAndKeepDownloaded` / `downloadLazily` / `inherited`; the choice is part of the
  metadata version) and, for `online`, calls `evictItem` for every file below it. Folders macOS
  has loaded stay: evicting one makes it dataless, and an open Finder window then shows it
  empty until it is opened again. Eviction is retried for 20 s on any error: until macOS has
  taken the new policy a file that was kept fails with `-2008` (surfaced as NSCocoaErrorDomain
  256). macOS does the downloading itself. Items carry `allowsEvicting`, without it the Finder has no "Remove
  Download". `VfsMac::availability` shows the choice, otherwise the real state of the file in
  the CloudStorage folder (`SF_DATALESS`, read with materialization switched off for the
  thread; for folders down through loaded subfolders, which stay loaded after freeing, until
  both states were seen or 20,000 entries were read), so choices stored by 11.0.0 cannot show a
  wrong state; kept items are sent again at every start. The folder view refreshes the row, its
  loaded rows and the folders above it 3 s, 15 s, 1, 3, 10 and 30 min after a choice, and
  shows at most 500 file rows per folder. macOS takes new item properties for known items only with a new metadata version and
  a full enumeration: `FileProviderItem.traitsVersion` is part of the metadata version, and
  the store answers `syncAnchorExpired` once when it differs from the stored value (bump it
  when capabilities change). Tested 2026-11-05 on macOS 27.2 with the real client menu and
  Finder: keep (recursive), server changes in a kept folder arrive by themselves, free up
  (recursive), reset, "Remove Download" in the Finder also for items known before the update, a
  folder choice over a different choice below it, in both directions.
- **Large folders**: macOS creates a new enumerator for every page of an enumeration, so the
  listing for the following pages lives in `DomainContext` (`keepListing`, 120 s since its last
  use), not in the enumerator; otherwise every page fetches the whole folder again and rewrites
  the state. Measured 2026-11-06 on macOS 27.2 with 20,000 files in one folder: first listing
  40 s (before 16 min 40 s), extension at most 72 MB; "Always keep" 17.5 min, "Free up space"
  1.5 min. While `fileproviderd` takes in such a folder or applies a choice to it, other
  changes in the domain wait for a few minutes, nothing is lost. Not tested: a file deleted on
  the server between two pages of a first listing, if a change walk runs in between, could come
  back from the kept listing until it is opened.
- **Recursive delete right after a change**: a recursive delete is rejected when the folder
  changed on the server since macOS last saw it, so nothing unseen goes with it, and the folder
  comes back. This also hits a change of one's own within the last seconds, until the change
  enumeration has reported the new etag. Deleting again works, nothing is lost.
- **GUI**: wizard and folder wizard offer virtual files on macOS 13+ when the extension is
  embedded (no environment variable), and **preselect** them: a new account syncs as
  virtual files ("Sync and download files as you use them"), classic sync is the
  alternative. Existing folders keep their mode; the folder menu switches them
  ("Activate virtual files" / "Deactivate virtual files"). A File Provider folder keeps only its journal in
  `~/Library/Application Support/owncloud.online/FileProvider/<uuid>`.

Build / test on this Mac: Craft (`--compile`, `--install --qmerge`) with
`BUILD_MAC_FILE_PROVIDER=ON` and `SOCKETAPI_TEAM_IDENTIFIER_PREFIX=4545KLA52K.`, then
`SIGN_IDENTITY="Apple Development: …" admin/osx/make_release.sh` (or the Developer ID
default), install to `/Applications`, `lsregister -f` + `pluginkit -a/-e use`. The extension
logic can be driven outside the system with a harness that compiles the extension sources
against a WebDAV mock; inside the system with `~/ocvfsprobe` (sandboxed driver app whose
extension target takes the same sources).

Since 11.0.0 the CI dmg embeds the extension, signed with its entitlements and notarized
(Developer ID of team A7VZ8HFNR4, app group `A7VZ8HFNR4.online.owncloud.desktopclient`).

Fixed in the re-release of 11.0.0 (2026-11-04): a folder created on the Mac was never enumerated by macOS, so it never
became a "known" folder and server changes inside it did not arrive. `createItem` now gives a
new folder an empty baseline, and an adopted server folder (same name, `mayAlreadyExist`)
gets an empty remembered ETag in its parent, so the next change walk compares it once in
full. Harness: 50 checks (5 new ones fail with the first 11.0.0 build).

Tested on 2026-11-03 against testoo.bw.tech (owncloud.online 11.0.46, OAuth2, account
qa-test) with a locked GUI session, so the wizard could not be driven: the account was
written like the wizard does (OAuth refresh token in the keychain, folder with
`virtualFilesMode=macfileprovider`). Client side, seen in its log: domain registered, token
refreshed at start, after a restart and while running (the extension gets one 401 with the
old token, asks, and goes on with the mirrored one), ETag poll finds server changes within
30 s and the extension reports them, a restart keeps the domain. Finder-like operations ran
through the probe app (same extension sources, domain on a test folder), because a shell may
not open the client's CloudStorage folder (TCC `kTCCServiceFileProviderDomain`): listing,
hydration, create, rename, move, folder rename, delete (server trash bin), recursive folder
delete, 1 GB upload as one PUT (server SHA1 matches) and download, edit conflict (both
versions kept), delete of a file changed on the server (rejected, server version comes
back), offline → online through a local proxy (pending changes uploaded after
`signalErrorResolved`). Found and fixed: a domain left over without its folder blocked a new
domain with the same display name. Not evictable (`-2008`, fileproviderd: "content policy
prevention"): files written by the sandboxed probe app, probably because of their quarantine
xattr, which the extension does not sync; files from the server evict fine.

> This doc is a handoff for a Claude Code session running **on a Mac** (Xcode + CMake +
> Qt 6.8 + an Apple Developer account for signing). It captures the ownCloud-side
> architecture we already know and the plan; the exact Nextcloud code must be fetched +
> studied on the Mac (see References).

## 0. Reality check (read first)

macOS virtual files are architecturally **very different** from Windows CfApi:

- **Windows (CfApi, current):** the `vfs/win` plugin runs **in-process** in the main
  client. The client's own `SyncEngine` drives it; the plugin just creates/hydrates
  placeholders via `CfApi`. See `src/plugins/vfs/win/vfs_win.cpp` (~2200 lines).
- **macOS (FileProvider):** Apple requires a **separate File Provider extension**
  (its own process + app bundle, `NSFileProviderReplicatedExtension`). The extension
  is what the Finder talks to; it must enumerate items, serve content on demand, and
  accept changes. In Nextcloud the extension runs **its own sync/download logic** and
  talks to the main app over a local **XPC/line-based socket** ("Client Communication
  Service"). So this is not "implement 14 Vfs methods in-process" — it is **a new
  extension target that hosts sync logic + IPC to the main app**, plus a thin
  `vfs/mac` shim on the app side.

Consequence: this is a multi-day, multi-thousand-line feature. Plan for iterative
build/test/sign cycles on the Mac. Do NOT expect a first-compile success.

## 1. ownCloud-side architecture (already verified in this repo)

### Vfs abstract interface — `src/common/vfs.h`
`class Vfs : public QObject` — pure-virtuals a backend must implement:
`mode()`, `stop()`, `unregisterFolder()`, `socketApiPinStateActionsShown()`,
`createPlaceholder(item)`, `needsMetadataUpdate(item)`, `isDehydratedPlaceholder(path)`,
`statTypeVirtualFile(stat, data)`, `setPinState(relPath, state)`, `pinState(relPath)`,
`availability(folderPath)`, `fileStatusChanged(name, status)`,
`updateMetadata(item, path, replacesFile)`, `startImpl(params)`.
Plus `VfsPluginManager` (bestAvailableVfsMode / isVfsPluginAvailable / createVfsFromPlugin).

### Vfs mode enum — `src/common/vfs.h:112`
```cpp
enum Mode { Off, WindowsCfApi };   // <-- add a macOS mode, e.g. MacFileProvider
```
Update: `Vfs::modeFromString/modeToString`, `Utility::enumToString(Vfs::Mode)`,
`bestAvailableVfsMode()` (return the mac mode on macOS when the extension is installed),
and the wizard gating (`advancedsettingspagecontroller.cpp`, `folderwizard.cpp`,
`Theme::forceVirtualFilesOption()` — already true for owncloud.online).

### Plugin build — `src/plugins/vfs/CMakeLists.txt`
Iterates `VIRTUAL_FILE_SYSTEM_PLUGINS` (currently `off`, `win`). Add `mac` there,
gated on `APPLE`. Each plugin is a shared lib loaded via `VfsPluginManager`.

### Existing macOS shell integration — `shell_integration/MacOSX/`
Already has the Finder Sync extension (context menu / socket API) — this is NOT the
File Provider extension, but it's where the new FileProvider extension target lives
alongside, and both share the App Group + the socket to the main app.

## 2. Nextcloud reference (study on the Mac)

Clone `github.com/nextcloud/desktop` and read:
- `shell_integration/MacOSX/NextcloudIntegration/FileProviderExt/` — the Swift
  `NSFileProviderReplicatedExtension`: `FileProviderExtension.swift`, `Item/`,
  `Enumerator/`, `FileProviderData`, domain setup. **This is the bulk of the work.**
- `src/gui/macOS/` — app-side: `fileprovider*.{h,mm}`, domain management
  (`NSFileProviderManager` add/remove domains per account), settings UI, the XPC/service
  bridge (`ClientCommunicationService`, `LineProcessor`).
- `src/libsync/vfs/` + how the extension reuses libsync (the extension links a sync core).
- The Xcode/CMake glue that builds + embeds + signs the appex into the .app.
- Entitlements: `com.apple.developer.fileprovider.testing-mode` (dev),
  App Group `group.<teamid>.<bundle>`, `com.apple.security.application-groups`.

Map Nextcloud concepts to ownCloud: **account model differs** (ownCloud has Spaces /
a different `Account`/`Folder` model). The FileProvider "domain" should map to an
ownCloud account (+ space). Expect real porting, not copy-paste.

## 3. Deliverables / file map (to create on the Mac)

1. `src/plugins/vfs/mac/` — app-side Vfs shim:
   `vfs_mac.h/.mm`, `vfs_mac_plugin_factory.mm`, `CMakeLists.txt`. Implements the Vfs
   interface by driving `NSFileProviderManager` (register/unregister a domain per folder)
   and translating pin-state/availability/placeholder calls; delegates actual on-demand
   IO to the extension.
2. `shell_integration/MacOSX/OwncloudIntegration/FileProviderExt/` — the
   `NSFileProviderReplicatedExtension` (Swift): extension principal, item model,
   enumerator, content fetch, change application. Hosts/links the sync logic.
3. App-side domain + IPC: `src/gui/macOS/fileprovider*.{h,mm}` — domain lifecycle,
   the socket/XPC bridge to the extension, settings toggle.
4. `Vfs::Mode` + `bestAvailableVfsMode()` + wizard gating for macOS.
5. Entitlements + Info.plist + App Group + CMake/Xcode: build the appex, embed it in
   `owncloud.online.app/Contents/PlugIns/`, codesign both with the App Group.

## 4. Signing / entitlements (Apple Developer account required)

- App Group shared by app + appex (state/socket): `group.<TEAMID>.owncloud.online`.
- Extension entitlement `com.apple.developer.fileprovider.testing-mode` for local dev
  (no per-user provisioning) OR a proper provisioning profile for distribution.
- Both binaries hardened-runtime signed with the same Team ID; notarize for release.

## 5. Build / test loop on the Mac

1. `cmake` the client with Qt 6.8 (macOS), `-DVIRTUAL_FILE_SYSTEM_PLUGINS=...;mac`.
2. Build → the appex must land in `Contents/PlugIns` + be signed.
3. Run the app, connect an account, enable virtual files → the FileProvider domain
   registers → the account appears in Finder sidebar under Locations with dataless
   placeholders that hydrate on open.
4. Debug the extension via Console.app / attaching to the appex process.

## 6. Suggested order

1. Enum + gating + empty `vfs/mac` plugin that compiles + registers a FileProvider
   domain and shows the folder in Finder (no real IO yet). Prove the pipeline.
2. Enumerator + item model → placeholders visible.
3. Content fetch (hydration) + change application (upload).
4. Pin state / availability / eviction.
5. Multi-account / Spaces mapping, signing for distribution.

Ship it as an experimental/opt-in build first (like the Windows VFS rollout: 7.4.x).
