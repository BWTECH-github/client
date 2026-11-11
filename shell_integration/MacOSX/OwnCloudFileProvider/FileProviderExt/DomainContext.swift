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

import FileProvider
import os.log

/// Everything the extension and its enumerators share for one domain: the
/// connection, the item store and the lookups between identifiers, paths and
/// server entries.
final class DomainContext {
    let domain: NSFileProviderDomain
    let backend: DomainBackend
    let store: ItemStore
    /// was gerade hoch- und heruntergeladen wird, für die Ordneransicht des Clients
    let transfers: TransferBoard
    private let manager: NSFileProviderManager?
    private let lock = NSLock()
    private var signalScheduled = false
    /// Die Liste eines Ordners für die folgenden Seiten einer Auflistung. Das System legt je Seite einen
    /// neuen Enumerator an: Hielte der die Liste, holte jede Seite den ganzen Ordner neu vom Server und
    /// schriebe den Zustand neu. Bei 20.000 Dateien wären das 100 Durchgänge über alle Einträge.
    private var listings: [NSFileProviderItemIdentifier: (items: [FileProviderItem], used: Date)] = [:]
    private static let listingLifetime: TimeInterval = 120
    private let log = Logger(subsystem: logSubsystem, category: "fileprovider")

    init(domain: NSFileProviderDomain) {
        self.domain = domain
        backend = DomainBackend(domainIdentifier: domain.identifier.rawValue)
        store = ItemStore(domainIdentifier: domain.identifier.rawValue)
        transfers = TransferBoard(domainIdentifier: domain.identifier.rawValue)
        manager = NSFileProviderManager(for: domain)
    }

    func client() throws -> WebDavClient {
        try backend.client()
    }

    /// Where downloads land before the system takes them over: on the volume of
    /// the domain, so the system can move instead of copy.
    func temporaryDirectory() -> URL {
        if let url = try? manager?.temporaryDirectoryURL() {
            return url
        }
        return FileManager.default.temporaryDirectory
    }

    /// Path of an item, recovered from the server (file id -> path) if the
    /// store does not know it (yet), so items never become orphaned.
    func path(for identifier: NSFileProviderItemIdentifier, client: WebDavClient) async throws -> String {
        if let path = await store.path(for: identifier) {
            return path
        }
        if let recovered = try await client.resolvePath(fileId: identifier.rawValue) {
            await store.bind(id: identifier, path: recovered)
            return recovered
        }
        throw DavError.unknownItem
    }

    /// The current server entry of an item, also after it was renamed or moved
    /// on the server; nil if it no longer exists there.
    func entryIfExists(for identifier: NSFileProviderItemIdentifier, client: WebDavClient,
                       checksums: Bool = false) async throws -> DavEntry? {
        if identifier == .rootContainer {
            return try await client.stat(path: "", checksums: checksums)
        }
        if let known = await store.path(for: identifier),
           let entry = try await client.stat(path: known, checksums: checksums), entry.identifier == identifier {
            return entry
        }
        // moved, or something else lives at the old path now: ask the server
        if let resolved = try await client.resolvePath(fileId: identifier.rawValue),
           let entry = try await client.stat(path: resolved, checksums: checksums), entry.identifier == identifier {
            await store.bind(id: identifier, path: entry.path)
            return entry
        }
        return nil
    }

    func entry(for identifier: NSFileProviderItemIdentifier, client: WebDavClient,
               checksums: Bool = false) async throws -> DavEntry {
        guard let entry = try await entryIfExists(for: identifier, client: client, checksums: checksums) else {
            throw DavError.unknownItem
        }
        return entry
    }

    /// Identifier of the folder at `path`, looked up on the server if needed (a
    /// folder the system has not enumerated yet).
    func identifier(ofFolder path: String, client: WebDavClient) async throws -> NSFileProviderItemIdentifier {
        if let known = await store.identifier(forPath: path) {
            return known
        }
        guard let entry = try await client.stat(path: path) else {
            throw DavError.unknownItem
        }
        await store.register(entries: [entry])
        return entry.identifier
    }

    func item(for entry: DavEntry, client: WebDavClient) async throws -> FileProviderItem {
        let pin = await store.pin(for: entry.identifier)
        if entry.path.isEmpty {
            return FileProviderItem(entry: entry, parent: .rootContainer, pin: pin)
        }
        return FileProviderItem(entry: entry, parent: try await identifier(ofFolder: entry.parentPath, client: client), pin: pin)
    }

    func keepListing(_ items: [FileProviderItem], of container: NSFileProviderItemIdentifier) {
        lock.lock()
        defer { lock.unlock() }
        // was eine abgebrochene Auflistung liegen ließ
        listings = listings.filter { Date().timeIntervalSince($0.value.used) < Self.listingLifetime }
        listings[container] = (items, Date())
    }

    func keptListing(of container: NSFileProviderItemIdentifier) -> [FileProviderItem]? {
        lock.lock()
        defer { lock.unlock() }
        guard let kept = listings[container], Date().timeIntervalSince(kept.used) < Self.listingLifetime else {
            listings[container] = nil
            return nil
        }
        listings[container] = (kept.items, Date())
        return kept.items
    }

    func dropListing(of container: NSFileProviderItemIdentifier) {
        lock.lock()
        defer { lock.unlock() }
        listings[container] = nil
    }

    /// Let the system enumerate the working set's changes soon: after our own
    /// writes (the etags of the parent folders changed) and when the user looks
    /// at a folder. Coalesced, at most every two seconds.
    func signalWorkingSet() {
        lock.lock()
        let schedule = !signalScheduled
        signalScheduled = true
        lock.unlock()
        guard schedule else { return }
        DispatchQueue.global().asyncAfter(deadline: .now() + 2) { [weak self] in
            guard let self = self else { return }
            self.lock.lock()
            self.signalScheduled = false
            self.lock.unlock()
            self.manager?.signalEnumerator(for: .workingSet) { error in
                if let error = error {
                    self.log.error("Could not signal the working set: \(logDescription(error), privacy: .public)")
                }
            }
        }
    }
}

/// Eine Anfrage des Clients (Menü „Immer auf diesem Gerät behalten“ usw.): eine Datei je Anfrage im
/// Container der App-Gruppe, damit Client und Erweiterung nie dieselbe Datei gleichzeitig schreiben.
private struct PinRequest: Decodable {
    let path: String
    let state: String
}

extension DomainContext {
    /// Übernimmt die Anfragen des Clients in den ItemStore. Liefert die Objekte, deren Speicher
    /// freigegeben werden soll. Eine Anfrage bleibt liegen, wenn der Server gerade nicht antwortet.
    func takePinRequests(client: WebDavClient) async -> [NSFileProviderItemIdentifier] {
        guard let container = AppGroup.container,
              let files = try? FileManager.default.contentsOfDirectory(at: container, includingPropertiesForKeys: nil) else {
            return []
        }
        let prefix = "FileProviderPin-\(domain.identifier.rawValue)-"
        var toEvict: [NSFileProviderItemIdentifier] = []
        for file in files.filter({ $0.lastPathComponent.hasPrefix(prefix) }).sorted(by: { $0.lastPathComponent < $1.lastPathComponent }) {
            guard let data = try? Data(contentsOf: file), let request = try? JSONDecoder().decode(PinRequest.self, from: data) else {
                try? FileManager.default.removeItem(at: file)
                continue
            }
            do {
                if let entry = try await client.stat(path: request.path) {
                    await store.register(entries: [entry])
                    await store.setPin(request.state == PinChoice.reset ? nil : request.state, for: entry.identifier)
                    if request.state == PinChoice.free {
                        toEvict.append(contentsOf: await store.evictionTargets(of: entry.identifier))
                    }
                    log.info("Pin \(request.state, privacy: .public) for \(request.path, privacy: .private(mask: .hash))")
                }
                try? FileManager.default.removeItem(at: file)
            } catch {
                log.error("Pin request postponed: \(logDescription(error), privacy: .public)")
            }
        }
        return toEvict
    }

    /// Die Objekte mit geänderter Wahl, damit das System die neue contentPolicy übernimmt.
    func pinUpdates(client: WebDavClient) async throws -> (items: [FileProviderItem], ids: [String]) {
        var items: [FileProviderItem] = []
        var ids: [String] = []
        for id in await store.pinsWaitingForReport() {
            let identifier = NSFileProviderItemIdentifier(id)
            if let entry = try await entryIfExists(for: identifier, client: client) {
                items.append(try await item(for: entry, client: client))
                ids.append(id)
            } else {
                await store.dropPinReport(id)
            }
        }
        return (items, ids)
    }

    /// Gibt den Speicher frei, sobald das System die Wahl übernommen hat. Bis dahin gilt ein Objekt unter
    /// „immer behalten“ noch als nicht entfernbar, und die Freigabe eines Ordners bricht an ihm ab, mit
    /// wechselnden Fehlern (auch NSCocoaErrorDomain 256). Deshalb bei jedem Fehler einige Versuche.
    func evict(_ identifiers: [NSFileProviderItemIdentifier], attempt: Int = 1) {
        guard !identifiers.isEmpty, let manager = manager else { return }
        DispatchQueue.global().asyncAfter(deadline: .now() + (attempt == 1 ? 1 : 2)) { [weak self] in
            for identifier in identifiers {
                manager.evictItem(identifier: identifier) { error in
                    guard let self = self else { return }
                    guard let error = error else {
                        self.log.info("Space freed after \(attempt, privacy: .public) attempt(s)")
                        return
                    }
                    if attempt < 10 {
                        self.evict([identifier], attempt: attempt + 1)
                    } else {
                        let reason = (error as NSError).userInfo[NSUnderlyingErrorKey] as? Error
                        self.log.error("Could not free the space of an item: \(logDescription(error), privacy: .public) \(reason.map(logDescription) ?? "", privacy: .public)")
                    }
                }
            }
        }
    }
}

/// The result of a change walk over the working set.
struct ChangeSet {
    var updates: [FileProviderItem] = []
    var deletions: [NSFileProviderItemIdentifier] = []
    /// New baselines of the visited containers.
    var snapshots: [String: [String: Seen]] = [:]
    /// Known containers that are gone on the server.
    var removedContainers: [String] = []

    var isEmpty: Bool { updates.isEmpty && deletions.isEmpty }
}

extension DomainContext {
    /// What changed on the server since the baseline, for every container the
    /// system knows. The walk starts at the root and only descends into known
    /// folders whose etag changed: owncloud.online propagates etags to the
    /// root, so an unchanged folder has nothing new below it (the desktop
    /// client's discovery relies on the same rule). An id that vanished from a
    /// folder is looked up by file id, which tells a move from a deletion.
    func walkChanges() async throws -> ChangeSet {
        var result = ChangeSet()
        guard await store.isKnownContainer(.rootContainer) else {
            return result
        }
        let client = try client()
        let pins = await store.pins()
        var queue: [(NSFileProviderItemIdentifier, String)] = [(.rootContainer, "")]
        var visited = Set<String>()
        while !queue.isEmpty {
            let (container, path) = queue.removeFirst()
            guard visited.insert(container.rawValue).inserted else { continue }
            let entries: [DavEntry]
            do {
                entries = try await client.propfind(path: path, depth: 1)
            } catch DavError.http(404) {
                continue // gone; the diff of its parent reports it
            }
            let children = entries.filter { $0.path != path && !SyncRules.isExcluded($0.name) }
            let baseline = await store.baseline(of: container)
            var snapshot: [String: Seen] = [:]
            for child in children {
                let id = child.identifier
                snapshot[id.rawValue] = child.seen
                let before = baseline[id.rawValue]
                if before != child.seen {
                    result.updates.append(FileProviderItem(entry: child, parent: container, pin: pins[id.rawValue]))
                }
                if child.isFolder, before?.etag != child.etag || before == nil, await store.isKnownContainer(id) {
                    queue.append((id, child.path))
                }
            }
            await store.register(entries: children)

            for goneId in baseline.keys where snapshot[goneId] == nil {
                let ident = NSFileProviderItemIdentifier(goneId)
                if let movedPath = try await client.resolvePath(fileId: goneId),
                   let moved = try await client.stat(path: movedPath), moved.identifier == ident,
                   !SyncRules.isExcluded(moved.name) {
                    // moved on the server (a known new parent is visited as well, its etag changed)
                    await store.bind(id: ident, path: moved.path)
                    result.updates.append(try await item(for: moved, client: client))
                } else {
                    result.deletions.append(ident)
                    result.removedContainers.append(goneId)
                    result.removedContainers.append(contentsOf: await store.descendants(of: ident))
                    await store.forget(ident)
                }
            }
            result.snapshots[container.rawValue] = snapshot
        }

        // an item moved between two visited folders shows up twice: keep its last state
        var seenIds = Set<NSFileProviderItemIdentifier>()
        result.updates = result.updates.reversed().filter { seenIds.insert($0.itemIdentifier).inserted }.reversed()
        let updated = Set(result.updates.map(\.itemIdentifier))
        result.deletions = result.deletions.filter { !updated.contains($0) }
        return result
    }
}
