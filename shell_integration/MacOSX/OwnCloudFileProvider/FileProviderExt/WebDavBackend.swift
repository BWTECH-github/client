/*
 * Copyright (C) by BW-Tech GmbH
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

import CryptoKit
import FileProvider
import Foundation
import os.log
import Security

let logSubsystem = "online.owncloud.desktopclient"

/// The app group shared with the client, as declared in our Info.plist.
enum AppGroup {
    static let identifier: String? = {
        guard let ext = Bundle.main.object(forInfoDictionaryKey: "NSExtension") as? [String: Any] else { return nil }
        return ext["NSExtensionFileProviderDocumentGroup"] as? String
    }()

    static var container: URL? {
        guard let group = identifier else { return nil }
        return FileManager.default.containerURL(forSecurityApplicationGroupIdentifier: group)
    }

    /// Keychain service of the Authorization headers the client stores for us
    /// (same name in src/plugins/vfs/mac/fileproviderdomain.mm).
    static var keychainService: String? { identifier.map { "\($0).fileprovider" } }

    /// Darwin notification asking the client for fresh credentials after the
    /// server rejected ours (the client observes it, src/plugins/vfs/mac).
    static var credentialsRequest: String? { identifier.map { "\($0).fileprovider.credentials" } }
}

/// Non-secret connection settings for one File Provider domain.
///
/// The client writes them to `FileProviderConfig-<domain>.json` in the shared
/// app group container (src/plugins/vfs/mac) and rewrites them when the
/// account changes, e.g. after a server move. The secret - the value of the
/// Authorization header - is never part of this file; it lives in the
/// keychain, see `CredentialStore`.
struct DavConfig: Codable, Equatable {
    /// WebDAV URL of the domain root, e.g.
    /// `https://host/remote.php/dav/files/alice/` or a sub folder of it.
    let webDavUrl: String
    let username: String?
    /// Path of the domain root below the user's files root ("" or "Projects/2026"),
    /// used to map `oc:meta-path-for-user` answers onto domain paths.
    let remoteRoot: String?
    /// `…/remote.php/dav/meta`; derived from webDavUrl when absent.
    let metaUrl: String?
    let userAgent: String?

    var rootURL: URL? { URL(string: webDavUrl) }

    var metaBaseURL: URL? {
        if let explicit = metaUrl, let url = URL(string: explicit) { return url }
        guard let url = rootURL, var comps = URLComponents(url: url, resolvingAgainstBaseURL: false) else {
            return nil
        }
        // …/remote.php/dav/files/<user>[/sub]  ->  …/remote.php/dav/meta
        var parts = comps.path.split(separator: "/").map(String.init)
        guard let filesIdx = parts.firstIndex(of: "files"), filesIdx + 1 < parts.count else { return nil }
        parts.removeSubrange(filesIdx...)
        parts.append("meta")
        comps.path = "/" + parts.joined(separator: "/")
        return comps.url
    }

    /// Credentials over plain http would leak on the wire: only a loopback test
    /// server may use it, everything else must be https.
    var isTransportAcceptable: Bool {
        guard let url = rootURL else { return false }
        if url.scheme == "https" { return true }
        let host = url.host ?? ""
        return url.scheme == "http" && (host == "localhost" || host == "127.0.0.1" || host == "::1")
    }

    /// Percent-decoded path of the dav root without trailing slash, used to turn
    /// hrefs into domain-relative paths.
    var rootPath: String {
        guard let url = rootURL else { return "" }
        let path = url.path
        return path.hasSuffix("/") ? String(path.dropLast()) : path
    }

    /// Map a path relative to the user's files root (meta endpoint) onto this
    /// domain; nil if it lies outside the domain root.
    func domainPath(forUserPath userPath: String) -> String? {
        let trimmed = userPath.trimmingCharacters(in: CharacterSet(charactersIn: "/"))
        let prefix = (remoteRoot ?? "").trimmingCharacters(in: CharacterSet(charactersIn: "/"))
        if prefix.isEmpty { return trimmed }
        if trimmed == prefix { return "" }
        guard trimmed.hasPrefix(prefix + "/") else { return nil }
        return String(trimmed.dropFirst(prefix.count + 1))
    }

    static func fileURL(domainIdentifier: String) -> URL? {
        AppGroup.container?.appendingPathComponent("FileProviderConfig-\(domainIdentifier).json")
    }
}

/// The Authorization header value for a domain ("Bearer …" or "Basic …"),
/// stored by the client in the login keychain with an access list that names
/// the client and this extension (no provisioning profile needed, unlike the
/// data protection keychain).
///
/// The client owns the credentials: it signs in, refreshes OAuth2 tokens and
/// writes the header it last used successfully. The extension only reads it -
/// it never sees a refresh token, so app and extension can never race for one.
final class CredentialStore {
    private let domainIdentifier: String
    private let lock = NSLock()
    private var cached: String?
    private var lastRequest = Date.distantPast
    private let log = Logger(subsystem: logSubsystem, category: "credentials")

    /// A keychain prompt would block an extension nobody looks at: fail instead
    /// (errSecAuthFailed), e.g. if the access list names an older signature.
    private static let noKeychainUI: Void = {
        _ = SecKeychainSetUserInteractionAllowed(false)
    }()

    init(domainIdentifier: String) {
        self.domainIdentifier = domainIdentifier
        _ = Self.noKeychainUI
    }

    /// The header to use, read from the keychain on first use.
    func current() -> String? {
        lock.lock()
        defer { lock.unlock() }
        if cached == nil {
            cached = read()
        }
        return cached
    }

    /// The server rejected `rejected` (or there was none): re-read the keychain,
    /// and if it still holds the same value ask the client for fresh credentials
    /// (it refreshes the token, or removes them when the user signed out) and wait
    /// a moment for them. nil if nothing better arrives.
    func renewed(after rejected: String?, timeout: TimeInterval = 12) async -> String? {
        if let fresh = reload(), fresh != rejected {
            return fresh
        }
        requestFromClient()
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            try? await Task.sleep(nanoseconds: 500_000_000)
            if let fresh = reload(), fresh != rejected {
                log.info("Received new credentials from the client")
                return fresh
            }
        }
        return nil
    }

    private func reload() -> String? {
        lock.lock()
        defer { lock.unlock() }
        cached = read()
        return cached
    }

    private func requestFromClient() {
        lock.lock()
        let now = Date()
        let due = now.timeIntervalSince(lastRequest) > 5
        if due { lastRequest = now }
        lock.unlock()
        guard due, let name = AppGroup.credentialsRequest else { return }
        log.info("Asking the client for fresh credentials")
        CFNotificationCenterPostNotification(CFNotificationCenterGetDarwinNotifyCenter(),
                                             CFNotificationName(name as CFString), nil, nil, true)
    }

    private func read() -> String? {
        guard let service = AppGroup.keychainService else { return nil }
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: domainIdentifier,
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne,
        ]
        var result: CFTypeRef?
        let status = SecItemCopyMatching(query as CFDictionary, &result)
        guard status == errSecSuccess, let data = result as? Data, let value = String(data: data, encoding: .utf8) else {
            if status != errSecItemNotFound {
                log.error("Keychain read failed: \(status, privacy: .public)")
            }
            return nil
        }
        return value
    }
}

/// Files that are never synced, mirroring the client's sync-exclude.lst for
/// the names macOS and office suites create on their own.
enum SyncRules {
    private static let exactNames: Set<String> = [
        ".DS_Store", ".localized", "Icon\r", ".TemporaryItems", ".Trashes", ".fseventsd",
        ".Spotlight-V100", ".DocumentRevisions-V100", ".VolumeIcon.icns", "desktop.ini", "Thumbs.db",
        ".owncloudsync.log", ".sync-exclude.lst",
    ]

    static func isExcluded(_ name: String) -> Bool {
        if exactNames.contains(name) { return true }
        if name.hasPrefix("._") { return true } // AppleDouble
        if name.hasPrefix("~$") { return true } // Microsoft Office owner files
        if name.hasPrefix(".~lock.") && name.hasSuffix("#") { return true } // LibreOffice locks
        if name.hasPrefix(".sync_") && (name.hasSuffix(".db") || name.hasSuffix(".db-wal") || name.hasSuffix(".db-shm")) {
            return true // client journals
        }
        if name.hasSuffix(".~tmp") { return true }
        return false
    }

    /// "name (conflicted copy 2026-11-03 151200).ext", the client's own pattern
    /// (Utility::makeConflictFileName), so conflict files look alike on all platforms.
    static func conflictName(for name: String, date: Date = Date()) -> String {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "yyyy-MM-dd HHmmss"
        let marker = " (conflicted copy \(formatter.string(from: date)))"
        let ns = name as NSString
        let ext = ns.pathExtension
        // ".hidden" has no extension in this sense
        if ext.isEmpty || ns.deletingPathExtension.isEmpty {
            return name + marker
        }
        return ns.deletingPathExtension + marker + "." + ext
    }
}

/// One entry of a PROPFIND response.
struct DavEntry {
    /// Path relative to the domain root, "" for the root itself.
    let path: String
    let name: String
    let isFolder: Bool
    let size: Int64
    let etag: String
    let modified: Date?
    /// Server-stable id (oc:fileid). Survives rename/move, which is exactly
    /// what NSFileProviderItemIdentifier needs.
    let fileId: String
    /// oc:permissions, e.g. "RDNVCK"; empty if the server did not send any.
    let permissions: String
    /// oc:checksums, e.g. "SHA1:… MD5:… ADLER32:…"; only requested where needed.
    let checksums: String

    var identifier: NSFileProviderItemIdentifier {
        if path.isEmpty {
            return .rootContainer
        }
        return NSFileProviderItemIdentifier(fileId.isEmpty ? "p:\(path)" : fileId)
    }

    var parentPath: String {
        guard let slash = path.lastIndex(of: "/") else { return "" }
        return String(path[path.startIndex..<slash])
    }

    var sha1: String? {
        for part in checksums.split(separator: " ") where part.uppercased().hasPrefix("SHA1:") {
            return String(part.dropFirst(5)).lowercased()
        }
        return nil
    }

    var seen: Seen { Seen(etag: etag, name: name) }

    /// The same entry describing other content (the version we uploaded or downloaded).
    func with(etag newEtag: String, size newSize: Int64? = nil) -> DavEntry {
        DavEntry(path: path, name: name, isFolder: isFolder, size: newSize ?? size, etag: newEtag,
                 modified: modified, fileId: fileId, permissions: permissions, checksums: checksums)
    }
}

/// An error for the log without the user's data: URL errors carry the request
/// URL (server, user name, path), so only their domain and code go there.
func logDescription(_ error: Error) -> String {
    if let dav = error as? DavError {
        return String(describing: dav)
    }
    let ns = error as NSError
    return "\(ns.domain) \(ns.code)"
}

enum DavError: Error {
    /// No (usable) connection settings from the client yet.
    case notConfigured
    case insecureTransport
    /// No (valid) credentials from the client (yet), or the user signed out.
    case notAuthenticated
    case http(Int)
    case parse
    case unknownItem
    /// Server rejected a conditional write (If-Match/If-None-Match/Overwrite: F) -
    /// the item changed or exists.
    case precondition
    /// The downloaded content does not match the checksum the server sent.
    case checksumMismatch
}

func joinPath(_ parent: String, _ name: String) -> String {
    parent.isEmpty ? name : "\(parent)/\(name)"
}

private func unescapeXML(_ text: String) -> String {
    text.replacingOccurrences(of: "&lt;", with: "<")
        .replacingOccurrences(of: "&gt;", with: ">")
        .replacingOccurrences(of: "&quot;", with: "\"")
        .replacingOccurrences(of: "&apos;", with: "'")
        .replacingOccurrences(of: "&amp;", with: "&")
}

/// What the system was last told about a child of a container.
/// Wahl des Nutzers für ein Objekt (Client-Menü), wie sie in den Anfragen und im ItemStore steht.
enum PinChoice {
    static let keep = "local"
    static let free = "online"
    static let reset = "inherited"
}

struct Seen: Codable, Equatable {
    var etag: String
    var name: String
}

/// Persistent bookkeeping for one domain, a small JSON file in the app group
/// container (the client removes it together with the domain):
///  - id <-> path of every item the system knows,
///  - for every container the system enumerated (the "known" containers, a
///    superset of the materialized ones) what the system was told about its
///    children, as the baseline for change enumeration,
///  - the sync anchor of that baseline.
///
/// Change enumeration is two-phase: a batch of changes is staged as "pending"
/// under the next anchor and only becomes the baseline once the system asks for
/// changes from that anchor, i.e. once it has taken the batch. If it asks from
/// the older anchor again, the batch is computed afresh - no change is lost.
actor ItemStore {
    private struct Pending: Codable {
        var anchor: Int
        var snapshots: [String: [String: Seen]]
        var removedContainers: [String]
        var pinsReported: [String]?
    }

    private struct State: Codable {
        var format = 2
        var backingStore: String?
        var pathById: [String: String] = [:]
        var known: [String: [String: Seen]] = [:]
        var anchor = 1
        var pending: Pending?
        /// Set when the store was lost (didReset): the next full enumeration of the
        /// working set walks the whole tree to rebuild the baselines. Optional, so
        /// store files written before it existed still decode.
        var rebuild: Bool?
        /// Stand der Objekt-Eigenschaften (FileProviderItem.traitsVersion), den das System zuletzt
        /// vollständig bekommen hat. Fehlt in Store-Dateien von 11.0.0.
        var traits: Int?
        /// Wahl des Nutzers je Objekt (Kennung -> PinChoice), optional wegen älterer Store-Dateien.
        var pins: [String: String]?
        /// Objekte, deren geänderte Wahl das System noch nicht abgenommen hat.
        var pinsToReport: [String]?
    }

    private var state = State()
    private var idByPath: [String: String] = [:]
    private let file: URL?
    /// True when a store file existed but could not be read back - we then lost
    /// our delta baseline and must ask the system to re-enumerate from scratch.
    private(set) var didReset = false
    /// Einmalig nach einem Update der Erweiterung: das System soll alles neu aufzählen.
    private(set) var needsFullReport = false

    init(domainIdentifier: String) {
        let url = AppGroup.container?.appendingPathComponent("FileProviderStore-\(domainIdentifier).json")
        file = url
        var loaded = State()
        var reset = false
        if let url = url, let data = try? Data(contentsOf: url) {
            if let decoded = try? JSONDecoder().decode(State.self, from: data), decoded.format == 2 {
                loaded = decoded
            } else {
                reset = true // corrupt or from an older version - the baseline is gone
            }
        }
        // Neue Eigenschaften (z. B. „entfernbar“) übernimmt das System nur mit einer neuen Metadaten-Version
        // und einer vollständigen Aufzählung: der Ausgangsstand bleibt, das System zählt nur alles neu auf.
        needsFullReport = !reset && loaded.traits != FileProviderItem.traitsVersion
        state = loaded
        didReset = reset
        var reverse: [String: String] = [:]
        for (id, path) in loaded.pathById { reverse[path] = id }
        idByPath = reverse
    }

    private func save() {
        guard let file = file, let data = try? JSONEncoder().encode(state) else { return }
        try? data.write(to: file, options: .atomic)
    }

    private func startOver() {
        state = State(backingStore: state.backingStore, anchor: state.anchor + 1)
        idByPath = [:]
    }

    /// The system dropped its copy of the domain (new backing store): our
    /// baseline describes a replica that no longer exists.
    func adoptBackingStore(_ identity: Data?) {
        let value = identity?.base64EncodedString()
        guard value != state.backingStore else { return }
        if state.backingStore != nil {
            startOver()
        }
        state.backingStore = value
        save()
    }

    /// After a reset the system re-enumerates: start a fresh baseline with a new anchor.
    func clearFullReport() {
        needsFullReport = false
        state.traits = FileProviderItem.traitsVersion
        save()
    }

    func clearReset() {
        guard didReset else { return }
        startOver()
        state.rebuild = true
        didReset = false
        save()
    }

    /// The baselines were lost while the system still has items on disk: the next
    /// full working set enumeration must cover the whole tree (see clearReset()).
    var needsRebuild: Bool { state.rebuild == true }

    func finishRebuild() {
        guard state.rebuild != nil else { return }
        state.rebuild = nil
        save()
    }

    // MARK: id <-> path

    func path(for identifier: NSFileProviderItemIdentifier) -> String? {
        if identifier == .rootContainer { return "" }
        if let path = state.pathById[identifier.rawValue] { return path }
        if identifier.rawValue.hasPrefix("p:") { return String(identifier.rawValue.dropFirst(2)) }
        return nil
    }

    func identifier(forPath path: String) -> NSFileProviderItemIdentifier? {
        if path.isEmpty { return .rootContainer }
        return idByPath[path].map { NSFileProviderItemIdentifier($0) }
    }

    private func setPath(_ id: String, _ newPath: String) {
        if let old = state.pathById[id], old != newPath {
            // move the item and all descendants
            let prefix = old + "/"
            for (other, p) in state.pathById where other != id && p.hasPrefix(prefix) {
                let moved = newPath + "/" + String(p.dropFirst(prefix.count))
                if idByPath[p] == other { idByPath.removeValue(forKey: p) }
                state.pathById[other] = moved
                idByPath[moved] = other
            }
            if idByPath[old] == id { idByPath.removeValue(forKey: old) }
        }
        // another id that used to live here is gone or moved: forget its stale binding
        if let previous = idByPath[newPath], previous != id, state.pathById[previous] == newPath {
            state.pathById.removeValue(forKey: previous)
        }
        state.pathById[id] = newPath
        idByPath[newPath] = id
    }

    /// Forget the binding of an item and everything below it; returns the ids
    /// that were dropped (the item first).
    private func dropBinding(_ id: String) -> [String] {
        state.pins?.removeValue(forKey: id)
        guard let path = state.pathById.removeValue(forKey: id) else { return [id] }
        if idByPath[path] == id { idByPath.removeValue(forKey: path) }
        var dropped = [id]
        let prefix = path + "/"
        for (other, p) in state.pathById where p.hasPrefix(prefix) {
            state.pathById.removeValue(forKey: other)
            if idByPath[p] == other { idByPath.removeValue(forKey: p) }
            state.pins?.removeValue(forKey: other)
            dropped.append(other)
        }
        return dropped
    }

    /// Bind listed entries to their paths (a changed path moves descendants too).
    func register(entries: [DavEntry]) {
        for entry in entries where !entry.path.isEmpty {
            setPath(entry.identifier.rawValue, entry.path)
        }
        save()
    }

    func bind(id: NSFileProviderItemIdentifier, path: String) {
        guard id != .rootContainer else { return }
        setPath(id.rawValue, path)
        save()
    }

    /// Ids bound below the item (descendants), e.g. to retire their baselines.
    func descendants(of id: NSFileProviderItemIdentifier) -> [String] {
        guard let path = state.pathById[id.rawValue] else { return [] }
        let prefix = path + "/"
        return state.pathById.compactMap { $0.value.hasPrefix(prefix) ? $0.key : nil }
    }

    /// The item is gone on the server: forget it and everything below it.
    func forget(_ id: NSFileProviderItemIdentifier) {
        _ = dropBinding(id.rawValue)
        save()
    }

    // MARK: pins

    func pins() -> [String: String] { state.pins ?? [:] }

    func pin(for id: NSFileProviderItemIdentifier) -> String? { state.pins?[id.rawValue] }

    func pinsWaitingForReport() -> [String] { state.pinsToReport ?? [] }

    /// Merkt sich die Wahl für ein Objekt. Sie gilt für alles darunter: frühere Wahl dort entfällt.
    func setPin(_ choice: String?, for id: NSFileProviderItemIdentifier) {
        var pins = state.pins ?? [:]
        var report = state.pinsToReport ?? []
        var changed = false
        // auch wenn die Wahl am Objekt selbst schon so stand: darunter kann eine andere gelten
        for child in descendants(of: id) where pins[child] != nil {
            pins[child] = nil
            if !report.contains(child) { report.append(child) }
            changed = true
        }
        if pins[id.rawValue] != choice {
            pins[id.rawValue] = choice
            if !report.contains(id.rawValue) { report.append(id.rawValue) }
            changed = true
        }
        guard changed else { return }
        state.pins = pins
        state.pinsToReport = report
        save()
    }

    /// Was beim Freigeben entfernt wird: die Dateien. Ein Ordner, den macOS schon geladen hat, bleibt stehen.
    /// Entfernt man ihn mit, ist er leer, bis ihn jemand neu öffnet: ein offenes Finder-Fenster zeigt dann
    /// keine Dateien mehr. Ein Ordner, den macOS nie geladen hat, ist ohnehin frei.
    func evictionTargets(of id: NSFileProviderItemIdentifier) -> [NSFileProviderItemIdentifier] {
        guard isKnownContainer(id) else { return [id] }
        return descendants(of: id).filter { state.known[$0] == nil }.map { NSFileProviderItemIdentifier($0) }
    }

    func dropPinReport(_ id: String) {
        state.pinsToReport?.removeAll { $0 == id }
        save()
    }

    // MARK: baselines

    func isKnownContainer(_ container: NSFileProviderItemIdentifier) -> Bool {
        state.known[container.rawValue] != nil
    }

    /// Every container the system enumerated, with its current path, parents first.
    func knownContainers() -> [(NSFileProviderItemIdentifier, String)] {
        var result: [(NSFileProviderItemIdentifier, String)] = []
        for id in state.known.keys {
            let ident = NSFileProviderItemIdentifier(id)
            if let path = path(for: ident) { result.append((ident, path)) }
        }
        return result.sorted { $0.1.count < $1.1.count }
    }

    func baseline(of container: NSFileProviderItemIdentifier) -> [String: Seen] {
        state.known[container.rawValue] ?? [:]
    }

    /// The first enumeration of a container sets its baseline; later ones keep
    /// it, so the working set still reports what changed since (deletions too).
    func setBaselineIfUnknown(of container: NSFileProviderItemIdentifier, to snapshot: [String: Seen]) {
        if state.known[container.rawValue] == nil {
            state.known[container.rawValue] = snapshot
            save()
        }
    }

    /// The system already knows about a change we made for it (create, upload,
    /// rename, move): record it so change enumeration does not report it again.
    func noteChild(_ id: NSFileProviderItemIdentifier, seen: Seen, in container: NSFileProviderItemIdentifier,
                   previousContainer: NSFileProviderItemIdentifier? = nil) {
        if let previous = previousContainer, previous != container {
            state.known[previous.rawValue]?.removeValue(forKey: id.rawValue)
            state.pending?.snapshots[previous.rawValue]?.removeValue(forKey: id.rawValue)
        }
        state.known[container.rawValue]?[id.rawValue] = seen
        state.pending?.snapshots[container.rawValue]?[id.rawValue] = seen
        save()
    }

    /// An item the system deleted (or trashed) through us.
    func noteRemoval(_ id: NSFileProviderItemIdentifier, from container: NSFileProviderItemIdentifier?) {
        if let container = container {
            state.known[container.rawValue]?.removeValue(forKey: id.rawValue)
            state.pending?.snapshots[container.rawValue]?.removeValue(forKey: id.rawValue)
        }
        for dropped in dropBinding(id.rawValue) {
            state.known.removeValue(forKey: dropped)
            state.pending?.snapshots.removeValue(forKey: dropped)
        }
        save()
    }

    // MARK: anchors

    func currentAnchor() -> Int {
        state.anchor
    }

    /// Start a change enumeration from `anchor`: take a pending batch the system
    /// has acknowledged, drop one it has not. False if the anchor is unknown.
    func beginChanges(from anchor: Int) -> Bool {
        if let pending = state.pending, pending.anchor == anchor {
            for (container, snapshot) in pending.snapshots { state.known[container] = snapshot }
            for container in pending.removedContainers { state.known.removeValue(forKey: container) }
            if let reported = pending.pinsReported {
                state.pinsToReport?.removeAll { reported.contains($0) }
            }
            state.anchor = pending.anchor
            state.pending = nil
            save()
            return true
        }
        if anchor == state.anchor {
            if state.pending != nil {
                state.pending = nil
                save()
            }
            return true
        }
        return false
    }

    /// Stage the result of a change walk; returns the anchor to report.
    func stage(snapshots: [String: [String: Seen]], removedContainers: [String], pinsReported: [String] = []) -> Int {
        let next = state.anchor + 1
        state.pending = Pending(anchor: next, snapshots: snapshots, removedContainers: removedContainers, pinsReported: pinsReported)
        save()
        return next
    }
}

/// Minimal WebDAV client: PROPFIND/GET for reading, PUT/MKCOL/MOVE/DELETE for
/// writing. Every write is conditional (If-Match / If-None-Match / Overwrite: F)
/// so a concurrent change on the server is never silently overwritten.
final class WebDavClient {
    let config: DavConfig
    private let credentials: CredentialStore
    private let session: URLSession
    private let log = Logger(subsystem: logSubsystem, category: "webdav")

    init(config: DavConfig, credentials: CredentialStore) {
        self.config = config
        self.credentials = credentials
        let configuration = URLSessionConfiguration.default
        // No cookies: oC10 would treat a session cookie as a browser login and
        // demand a CSRF token on writes.
        configuration.httpShouldSetCookies = false
        configuration.httpCookieAcceptPolicy = .never
        configuration.httpCookieStorage = nil
        configuration.urlCache = nil
        configuration.requestCachePolicy = .reloadIgnoringLocalCacheData
        configuration.timeoutIntervalForRequest = 120
        if let agent = config.userAgent, !agent.isEmpty {
            configuration.httpAdditionalHeaders = ["User-Agent": agent]
        }
        session = URLSession(configuration: configuration)
    }

    /// Running transfers finish, then the session goes away (settings changed).
    func retire() {
        session.finishTasksAndInvalidate()
    }

    func url(for path: String) throws -> URL {
        guard var url = config.rootURL else { throw DavError.notConfigured }
        for component in path.split(separator: "/") {
            url.appendPathComponent(String(component))
        }
        return url
    }

    private func request(path: String, method: String, depth: String? = nil) throws -> URLRequest {
        var request = URLRequest(url: try url(for: path))
        request.httpMethod = method
        if let depth = depth {
            request.setValue(depth, forHTTPHeaderField: "Depth")
        }
        return request
    }

    /// Run a request with the client's credentials. After a 401 the client is
    /// asked for fresh ones (it may have refreshed the token in the meantime) and
    /// the request is repeated once. The extension never refreshes tokens itself.
    private func perform<T>(_ base: URLRequest,
                            _ run: (URLRequest) async throws -> (T, URLResponse)) async throws -> (T, URLResponse) {
        var auth = credentials.current()
        if auth == nil {
            auth = await credentials.renewed(after: nil)
        }
        guard let first = auth else { throw DavError.notAuthenticated }
        var request = base
        request.setValue(first, forHTTPHeaderField: "Authorization")
        let result = try await run(request)
        guard (result.1 as? HTTPURLResponse)?.statusCode == 401 else { return result }
        log.info("\(base.httpMethod ?? "", privacy: .public) rejected with 401, asking for fresh credentials")
        guard let fresh = await credentials.renewed(after: first) else { throw DavError.notAuthenticated }
        request.setValue(fresh, forHTTPHeaderField: "Authorization")
        let retried = try await run(request)
        if (retried.1 as? HTTPURLResponse)?.statusCode == 401 {
            throw DavError.notAuthenticated
        }
        return retried
    }

    /// A quoted etag for use in If-Match.
    private func quoted(_ etag: String) -> String {
        "\"\(etag.replacingOccurrences(of: "\"", with: ""))\""
    }

    private func expect(_ response: URLResponse, _ codes: Set<Int>, _ method: String, _ path: String) throws {
        guard let http = response as? HTTPURLResponse else { throw DavError.parse }
        guard codes.contains(http.statusCode) else {
            // paths are the user's data: hashed in the log, the same path gives the same hash
            log.error("\(method, privacy: .public) \(path, privacy: .private(mask: .hash)) -> HTTP \(http.statusCode)")
            if http.statusCode == 412 {
                throw DavError.precondition
            }
            throw DavError.http(http.statusCode)
        }
    }

    private static func propfindBody(checksums: Bool) -> Data {
        Data("""
        <?xml version="1.0"?>
        <d:propfind xmlns:d="DAV:" xmlns:oc="http://owncloud.org/ns">
          <d:prop>
            <d:resourcetype/>
            <d:getcontentlength/>
            <d:getetag/>
            <d:getlastmodified/>
            <oc:fileid/>
            <oc:permissions/>\(checksums ? "\n    <oc:checksums/>" : "")
          </d:prop>
        </d:propfind>
        """.utf8)
    }

    /// Resolve a server file id to its current domain path via the dav/meta
    /// endpoint. nil if the id no longer exists for the user (deleted, in the
    /// trash bin, unshared) or now lies outside this domain - used to tell a real
    /// deletion apart from a server-side move.
    func resolvePath(fileId: String) async throws -> String? {
        guard !fileId.isEmpty, !fileId.hasPrefix("p:"), fileId != NSFileProviderItemIdentifier.rootContainer.rawValue,
              let base = config.metaBaseURL
        else {
            return nil
        }
        var request = URLRequest(url: base.appendingPathComponent(fileId))
        request.httpMethod = "PROPFIND"
        request.setValue("0", forHTTPHeaderField: "Depth")
        request.setValue("application/xml", forHTTPHeaderField: "Content-Type")
        request.httpBody = Data("""
        <?xml version="1.0"?>
        <d:propfind xmlns:d="DAV:" xmlns:oc="http://owncloud.org/ns">
          <d:prop><oc:meta-path-for-user/></d:prop>
        </d:propfind>
        """.utf8)
        let (data, response) = try await perform(request) { try await self.session.data(for: $0) }
        guard let http = response as? HTTPURLResponse else { throw DavError.parse }
        if http.statusCode == 404 { return nil }
        guard http.statusCode == 207 else { throw DavError.http(http.statusCode) }
        // a missing value comes back empty (<oc:meta-path-for-user/>) in a 404 propstat
        guard let xml = String(data: data, encoding: .utf8),
              let range = xml.range(of: "meta-path-for-user>"),
              let end = xml.range(of: "</", range: range.upperBound..<xml.endIndex)
        else {
            return nil
        }
        return config.domainPath(forUserPath: unescapeXML(String(xml[range.upperBound..<end.lowerBound])))
    }

    /// PROPFIND on `path`; returns the entry itself first, children after.
    func propfind(path: String, depth: Int, checksums: Bool = false) async throws -> [DavEntry] {
        var request = try request(path: path, method: "PROPFIND", depth: String(depth))
        request.setValue("application/xml", forHTTPHeaderField: "Content-Type")
        request.httpBody = Self.propfindBody(checksums: checksums)
        let (data, response) = try await perform(request) { try await self.session.data(for: $0) }
        try expect(response, [207], "PROPFIND", path)
        return try MultistatusParser.parse(data: data, rootPath: config.rootPath)
    }

    /// The entry at `path`, nil if nothing is there.
    func stat(path: String, checksums: Bool = false) async throws -> DavEntry? {
        do {
            return try await propfind(path: path, depth: 0, checksums: checksums).first
        } catch DavError.http(404) {
            return nil
        }
    }

    /// Download a file into `directory`. The URLSession task is attached to
    /// `progress`, so cancelling the progress (Finder) cancels the download.
    /// Verifies the SHA1 the server announces in OC-Checksum.
    func fetch(path: String, into directory: URL, progress: Progress? = nil) async throws -> (URL, HTTPURLResponse) {
        let base = try request(path: path, method: "GET")
        let (tmp, response): (URL, URLResponse) = try await perform(base) { request in
            try await withCheckedThrowingContinuation { continuation in
                let task = self.session.downloadTask(with: request) { url, response, error in
                    if let error = error {
                        continuation.resume(throwing: error)
                    } else if let url = url, let response = response {
                        // Move synchronously inside the handler: url is deleted
                        // as soon as this closure returns.
                        let dst = directory.appendingPathComponent(UUID().uuidString)
                        do {
                            try FileManager.default.moveItem(at: url, to: dst)
                            continuation.resume(returning: (dst, response))
                        } catch {
                            continuation.resume(throwing: error)
                        }
                    } else {
                        continuation.resume(throwing: DavError.parse)
                    }
                }
                progress?.addChild(task.progress, withPendingUnitCount: progress?.totalUnitCount ?? 1)
                task.resume()
            }
        }
        do {
            try expect(response, [200], "GET", path)
            guard let http = response as? HTTPURLResponse else { throw DavError.parse }
            if let announced = http.value(forHTTPHeaderField: "OC-Checksum"), announced.uppercased().hasPrefix("SHA1:") {
                let expected = String(announced.dropFirst(5)).lowercased()
                if let actual = sha1Hex(of: tmp), actual != expected {
                    log.error("GET \(path, privacy: .private(mask: .hash)): the content does not match the server checksum")
                    throw DavError.checksumMismatch
                }
            }
            return (tmp, http)
        } catch {
            try? FileManager.default.removeItem(at: tmp)
            throw error
        }
    }

    /// Upload file contents (or an empty file). `ifMatch` guards against lost
    /// updates (send the base etag), `ifNoneMatch` = "*" refuses to overwrite an
    /// existing file (fresh creates). Returns the response (ETag, OC-FileId).
    @discardableResult
    func put(path: String, contents: URL?, modified: Date? = nil, sha1: String? = nil,
             ifMatch: String? = nil, ifNoneMatch: String? = nil) async throws -> HTTPURLResponse {
        var base = try request(path: path, method: "PUT")
        if let etag = ifMatch { base.setValue(quoted(etag), forHTTPHeaderField: "If-Match") }
        if let inm = ifNoneMatch { base.setValue(inm, forHTTPHeaderField: "If-None-Match") }
        if let modified = modified {
            // keep the modification time, as the desktop client does
            base.setValue(String(Int64(modified.timeIntervalSince1970)), forHTTPHeaderField: "X-OC-Mtime")
        }
        if let sha1 = sha1 {
            // the server keeps it and refuses a body that does not match
            base.setValue("SHA1:\(sha1)", forHTTPHeaderField: "OC-Checksum")
        }
        let (_, response) = try await perform(base) { request in
            if let contents = contents {
                return try await self.session.upload(for: request, fromFile: contents)
            }
            return try await self.session.upload(for: request, from: Data())
        }
        try expect(response, [200, 201, 204], "PUT", path)
        guard let http = response as? HTTPURLResponse else { throw DavError.parse }
        return http
    }

    /// Create a collection. Returns false if something already exists there (405).
    @discardableResult
    func mkcol(path: String) async throws -> Bool {
        let base = try request(path: path, method: "MKCOL")
        let (_, response) = try await perform(base) { try await self.session.data(for: $0) }
        if let http = response as? HTTPURLResponse, http.statusCode == 405 {
            return false
        }
        try expect(response, [201], "MKCOL", path)
        return true
    }

    /// Move/rename. Never overwrites an existing destination (Overwrite: F,
    /// the server answers 412).
    func move(path: String, to newPath: String) async throws {
        var base = try request(path: path, method: "MOVE")
        base.setValue(try url(for: newPath).absoluteString, forHTTPHeaderField: "Destination")
        base.setValue("F", forHTTPHeaderField: "Overwrite")
        let (_, response) = try await perform(base) { try await self.session.data(for: $0) }
        try expect(response, [201, 204], "MOVE", path)
    }

    /// Delete; owncloud.online keeps the item in the user's trash bin. `ifMatch`
    /// guards a file delete against a concurrent remote edit.
    func delete(path: String, ifMatch: String? = nil) async throws {
        var base = try request(path: path, method: "DELETE")
        if let etag = ifMatch { base.setValue(quoted(etag), forHTTPHeaderField: "If-Match") }
        let (_, response) = try await perform(base) { try await self.session.data(for: $0) }
        try expect(response, [204], "DELETE", path)
    }

    /// Map a transport/HTTP error onto the File Provider error the system expects.
    static func fileProviderError(for error: Error) -> Error {
        switch error {
        case DavError.unknownItem:
            return NSFileProviderError(.noSuchItem)
        case DavError.notAuthenticated, DavError.notConfigured:
            // the client signals the resolution once it has (new) settings and credentials
            return NSFileProviderError(.notAuthenticated)
        case DavError.insecureTransport:
            return NSFileProviderError(.cannotSynchronize)
        case DavError.precondition, DavError.checksumMismatch:
            // changed on the server / damaged on the way: the system retries
            return NSFileProviderError(.serverUnreachable)
        case DavError.http(let code):
            switch code {
            case 401:
                return NSFileProviderError(.notAuthenticated)
            case 403:
                return NSError(domain: NSCocoaErrorDomain, code: NSFileWriteNoPermissionError)
            case 404, 409:
                // 409: the parent folder is gone on the server
                return NSFileProviderError(.noSuchItem)
            case 507:
                return NSFileProviderError(.insufficientQuota)
            case 408, 423, 429, 500...599:
                return NSFileProviderError(.serverUnreachable) // retried by the system
            default:
                // other client errors (bad name, too large …) would fail the same way again
                return NSFileProviderError(.cannotSynchronize)
            }
        case let error as URLError where error.code == .cancelled:
            return CocoaError(.userCancelled)
        case is URLError:
            return NSFileProviderError(.serverUnreachable)
        case let error as NSError where error.domain == NSCocoaErrorDomain || error.domain == NSFileProviderErrorDomain:
            return error
        default:
            return NSFileProviderError(.serverUnreachable)
        }
    }
}

/// The connection of one domain: the client's settings (re-read whenever the
/// client rewrites them, e.g. after a server move) and the credentials.
final class DomainBackend {
    let domainIdentifier: String
    let credentials: CredentialStore
    private let lock = NSLock()
    private var loadedStamp: Date?
    private var cached: WebDavClient?
    private let log = Logger(subsystem: logSubsystem, category: "fileprovider")

    init(domainIdentifier: String) {
        self.domainIdentifier = domainIdentifier
        credentials = CredentialStore(domainIdentifier: domainIdentifier)
    }

    func client() throws -> WebDavClient {
        guard let file = DavConfig.fileURL(domainIdentifier: domainIdentifier) else { throw DavError.notConfigured }
        let stamp = (try? FileManager.default.attributesOfItem(atPath: file.path))?[.modificationDate] as? Date
        lock.lock()
        defer { lock.unlock() }
        if let client = cached, stamp != nil, stamp == loadedStamp {
            return client
        }
        guard let data = try? Data(contentsOf: file), let config = try? JSONDecoder().decode(DavConfig.self, from: data) else {
            throw DavError.notConfigured
        }
        guard config.isTransportAcceptable else {
            log.error("Refusing plain-http WebDAV for a non-local host; use https")
            throw DavError.insecureTransport
        }
        loadedStamp = stamp
        if let client = cached, client.config == config {
            return client
        }
        if cached != nil {
            log.info("Connection settings changed, now \(config.webDavUrl, privacy: .private(mask: .hash))")
        }
        cached?.retire()
        let client = WebDavClient(config: config, credentials: credentials)
        cached = client
        return client
    }
}

/// SHA1 of a local file, streamed.
func sha1Hex(of url: URL) -> String? {
    guard let handle = try? FileHandle(forReadingFrom: url) else { return nil }
    defer { try? handle.close() }
    var hasher = Insecure.SHA1()
    while true {
        let chunk = handle.readData(ofLength: 4 << 20)
        if chunk.isEmpty { break }
        hasher.update(data: chunk)
    }
    return hasher.finalize().map { String(format: "%02x", $0) }.joined()
}

/// Parses a DAV:multistatus document into entries.
private final class MultistatusParser: NSObject, XMLParserDelegate {
    private let rootPath: String
    private var entries: [DavEntry] = []

    private var href = ""
    private var isFolder = false
    private var size: Int64 = 0
    private var etag = ""
    private var fileId = ""
    private var permissions = ""
    private var checksums = ""
    private var modified: Date?
    private var text = ""

    private static let httpDate: DateFormatter = {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.timeZone = TimeZone(identifier: "GMT")
        f.dateFormat = "EEE, dd MMM yyyy HH:mm:ss zzz"
        return f
    }()

    private init(rootPath: String) {
        self.rootPath = rootPath
    }

    static func parse(data: Data, rootPath: String) throws -> [DavEntry] {
        let delegate = MultistatusParser(rootPath: rootPath)
        let parser = XMLParser(data: data)
        parser.delegate = delegate
        parser.shouldProcessNamespaces = true
        guard parser.parse() else { throw DavError.parse }
        return delegate.entries
    }

    func parser(_ parser: XMLParser, didStartElement elementName: String, namespaceURI: String?,
                qualifiedName qName: String?, attributes attributeDict: [String: String] = [:]) {
        text = ""
        if elementName == "collection" {
            isFolder = true
        }
        if elementName == "response" {
            href = ""
            isFolder = false
            size = 0
            etag = ""
            fileId = ""
            permissions = ""
            checksums = ""
            modified = nil
        }
    }

    func parser(_ parser: XMLParser, foundCharacters string: String) {
        text += string
    }

    func parser(_ parser: XMLParser, didEndElement elementName: String, namespaceURI: String?,
                qualifiedName qName: String?) {
        let value = text.trimmingCharacters(in: .whitespacesAndNewlines)
        switch elementName {
        case "href":
            href = value
        case "getcontentlength":
            size = Int64(value) ?? 0
        case "getetag":
            etag = value.replacingOccurrences(of: "\"", with: "")
        case "fileid":
            fileId = value
        case "permissions":
            permissions = value
        case "checksum":
            checksums += (checksums.isEmpty ? "" : " ") + value
        case "getlastmodified":
            modified = Self.httpDate.date(from: value)
        case "response":
            if let entry = makeEntry() {
                entries.append(entry)
            }
        default:
            break
        }
        text = ""
    }

    private func makeEntry() -> DavEntry? {
        guard let decoded = href.removingPercentEncoding else { return nil }
        var path = decoded
        if path.hasPrefix(rootPath) {
            path = String(path.dropFirst(rootPath.count))
        }
        path = path.trimmingCharacters(in: CharacterSet(charactersIn: "/"))
        let name = path.split(separator: "/").last.map(String.init) ?? ""
        return DavEntry(path: path, name: name, isFolder: isFolder, size: size, etag: etag,
                        modified: modified, fileId: fileId, permissions: permissions, checksums: checksums)
    }
}
