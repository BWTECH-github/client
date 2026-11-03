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
    private let manager: NSFileProviderManager?
    private let lock = NSLock()
    private var signalScheduled = false
    private let log = Logger(subsystem: logSubsystem, category: "fileprovider")

    init(domain: NSFileProviderDomain) {
        self.domain = domain
        backend = DomainBackend(domainIdentifier: domain.identifier.rawValue)
        store = ItemStore(domainIdentifier: domain.identifier.rawValue)
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
        if entry.path.isEmpty {
            return FileProviderItem(entry: entry, parent: .rootContainer)
        }
        return FileProviderItem(entry: entry, parent: try await identifier(ofFolder: entry.parentPath, client: client))
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
                    result.updates.append(FileProviderItem(entry: child, parent: container))
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
