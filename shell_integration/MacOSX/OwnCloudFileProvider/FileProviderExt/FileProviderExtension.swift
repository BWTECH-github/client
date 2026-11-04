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
import UniformTypeIdentifiers

/// The owncloud.online File Provider extension.
///
/// This is the process the Finder talks to: it owns one domain (one folder of
/// an account in the client) and serves the server tree over WebDAV - listing,
/// downloads on demand, uploads, renames, moves and deletions. The client
/// (src/plugins/vfs/mac) registers the domain, writes the connection settings,
/// keeps the credentials current and signals changes on the server.
///
/// Data safety: every write is conditional. Uploads carry If-Match with the
/// version the user edited (or If-None-Match for new files), moves never
/// overwrite (Overwrite: F), deletions of files carry If-Match and deletions of
/// folders are refused when the folder changed on the server since the system
/// last saw it. A conflicting edit is kept as a conflict copy next to the
/// server version, never dropped.
final class FileProviderExtension: NSObject, NSFileProviderReplicatedExtension {
    private let context: DomainContext
    private let log = Logger(subsystem: logSubsystem, category: "fileprovider")

    /// Fields we sync to the server; the rest (tags, last used date, …) stays local.
    private static let syncedFields: NSFileProviderItemFields = [.filename, .parentItemIdentifier, .contents]

    required init(domain: NSFileProviderDomain) {
        context = DomainContext(domain: domain)
        super.init()
        let store = context.store
        let identity = domain.backingStoreIdentity
        Task { await store.adoptBackingStore(identity) }
        log.info("Extension started for domain \(domain.identifier.rawValue, privacy: .public)")
    }

    func invalidate() {
        log.info("Extension invalidated for domain \(self.context.domain.identifier.rawValue, privacy: .public)")
    }

    // MARK: - Helpers

    /// The etag the system knows for an item; nil if it has none we gave it
    /// (beforeFirstSyncComponent: never in sync with the server).
    private static func etag(from version: NSFileProviderItemVersion?) -> String? {
        guard let data = version?.contentVersion, !data.isEmpty,
              data != NSFileProviderItemVersion.beforeFirstSyncComponent
        else {
            return nil
        }
        return String(data: data, encoding: .utf8)
    }

    private static func fileSize(_ url: URL?) -> Int64? {
        guard let url = url, let size = (try? FileManager.default.attributesOfItem(atPath: url.path))?[.size] as? NSNumber else {
            return nil
        }
        return size.int64Value
    }

    private static func responseEtag(_ response: HTTPURLResponse) -> String? {
        let value = (response.value(forHTTPHeaderField: "OC-ETag") ?? response.value(forHTTPHeaderField: "ETag"))?
            .replacingOccurrences(of: "\"", with: "")
        return value?.isEmpty == false ? value : nil
    }

    /// Our content just went up as `response`: the entry of the server item,
    /// describing exactly that version (someone may have changed it since).
    private func uploadedEntry(at path: String, response: HTTPURLResponse, contents: URL?,
                               client: WebDavClient) async throws -> DavEntry {
        guard let entry = try await client.stat(path: path) else {
            throw DavError.unknownItem
        }
        guard let etag = Self.responseEtag(response), etag != entry.etag else {
            return entry
        }
        context.signalWorkingSet() // the newer server version comes down afterwards
        return entry.with(etag: etag, size: Self.fileSize(contents) ?? 0)
    }

    /// Keep a local version the server cannot take (it changed there meanwhile):
    /// upload it as "name (conflicted copy <date>).ext" next to it, like the
    /// desktop client does. Never overwrites anything.
    private func uploadConflictCopy(of contents: URL, named name: String, in parentPath: String,
                                    modified: Date?, client: WebDavClient) async throws -> String {
        let sha1 = sha1Hex(of: contents)
        for attempt in 0..<5 {
            let conflictName = SyncRules.conflictName(for: name, date: Date().addingTimeInterval(TimeInterval(attempt)))
            let path = joinPath(parentPath, conflictName)
            do {
                try await client.put(path: path, contents: contents, modified: modified, sha1: sha1, ifNoneMatch: "*")
                log.info("Kept the local version as \(path, privacy: .private(mask: .hash))")
                context.signalWorkingSet()
                return path
            } catch DavError.precondition {
                continue
            }
        }
        throw DavError.precondition
    }

    private func collisionError(at path: String, client: WebDavClient) async throws -> Error {
        if let existing = try await client.stat(path: path) {
            return NSError.fileProviderErrorForCollision(with: try await context.item(for: existing, client: client))
        }
        return NSFileProviderError(.filenameCollision)
    }

    // MARK: - NSFileProviderReplicatedExtension

    func item(for identifier: NSFileProviderItemIdentifier,
              request: NSFileProviderRequest,
              completionHandler: @escaping (NSFileProviderItem?, Error?) -> Void) -> Progress {
        if identifier == .trashContainer || identifier == .workingSet {
            completionHandler(nil, NSFileProviderError(.noSuchItem))
            return Progress()
        }
        Task {
            do {
                let client = try context.client()
                let entry = try await context.entry(for: identifier, client: client)
                if !entry.path.isEmpty && SyncRules.isExcluded(entry.name) {
                    throw DavError.unknownItem
                }
                await context.store.register(entries: [entry])
                completionHandler(try await context.item(for: entry, client: client), nil)
            } catch {
                completionHandler(nil, WebDavClient.fileProviderError(for: error))
            }
        }
        return Progress()
    }

    func fetchContents(for itemIdentifier: NSFileProviderItemIdentifier,
                       version requestedVersion: NSFileProviderItemVersion?,
                       request: NSFileProviderRequest,
                       completionHandler: @escaping (URL?, NSFileProviderItem?, Error?) -> Void) -> Progress {
        let progress = Progress(totalUnitCount: 100)
        let task = Task {
            do {
                let client = try context.client()
                let entry = try await context.entry(for: itemIdentifier, client: client)
                guard !entry.isFolder else {
                    throw DavError.unknownItem
                }
                let (url, response) = try await client.fetch(path: entry.path, into: context.temporaryDirectory(), progress: progress)
                // the version we got: the file may have changed since the PROPFIND
                var fetched = entry
                if let etag = Self.responseEtag(response), etag != entry.etag {
                    fetched = entry.with(etag: etag, size: Self.fileSize(url) ?? entry.size)
                }
                completionHandler(url, try await context.item(for: fetched, client: client), nil)
            } catch {
                if Task.isCancelled || progress.isCancelled {
                    completionHandler(nil, nil, CocoaError(.userCancelled))
                } else {
                    log.error("Download of \(itemIdentifier.rawValue, privacy: .private(mask: .hash)) failed: \(logDescription(error), privacy: .public)")
                    completionHandler(nil, nil, WebDavClient.fileProviderError(for: error))
                }
            }
        }
        progress.cancellationHandler = { task.cancel() }
        return progress
    }

    func createItem(basedOn itemTemplate: NSFileProviderItem,
                    fields: NSFileProviderItemFields,
                    contents url: URL?,
                    options: NSFileProviderCreateItemOptions = [],
                    request: NSFileProviderRequest,
                    completionHandler: @escaping (NSFileProviderItem?, NSFileProviderItemFields, Bool, Error?) -> Void) -> Progress {
        let name = itemTemplate.filename
        let type = itemTemplate.contentType ?? .data
        // Names the client never syncs (.DS_Store, Office lock files, …) and
        // symbolic links stay on this Mac.
        if SyncRules.isExcluded(name) || type.conforms(to: .symbolicLink) {
            completionHandler(nil, [], false, NSFileProviderError(.excludedFromSync))
            return Progress()
        }
        let isFolder = type.conforms(to: .directory)
        let mayAlreadyExist = options.contains(.mayAlreadyExist)
        let modified = itemTemplate.contentModificationDate ?? nil
        let progress = Progress(totalUnitCount: 1)

        Task {
            do {
                let client = try context.client()
                // an unknown parent fails with noSuchItem: the system creates it first
                let parentPath = try await context.path(for: itemTemplate.parentItemIdentifier, client: client)
                let path = joinPath(parentPath, name)

                // rescan: den gemerkten ETag leeren, damit die nächste Änderungssuche den Ordner einmal
                // vollständig abgleicht (für einen übernommenen Server-Ordner, dessen Inhalt das System nicht kennt)
                func finish(_ entry: DavEntry, fetchContent: Bool = false, rescan: Bool = false) async throws {
                    let item = try await self.context.item(for: entry, client: client)
                    await self.context.store.register(entries: [entry])
                    let seen = rescan ? Seen(etag: "", name: entry.seen.name) : entry.seen
                    await self.context.store.noteChild(entry.identifier, seen: seen, in: item.parentItemIdentifier)
                    self.log.info("Created \(entry.isFolder ? "folder" : "file", privacy: .public) \(entry.path, privacy: .private(mask: .hash))")
                    completionHandler(item, [], fetchContent, nil)
                    self.context.signalWorkingSet()
                }

                if isFolder {
                    let created = try await client.mkcol(path: path)
                    guard let entry = try await client.stat(path: path) else { throw DavError.unknownItem }
                    if !created && !entry.isFolder {
                        completionHandler(nil, [], false, try await collisionError(at: path, client: client))
                        return
                    }
                    // Einen Ordner, den das System selbst anlegt, zählt es nie auf (es kennt seinen Inhalt).
                    // Ohne Ausgangsstand stiege die Änderungssuche nie in ihn hinein, und Dateien, die auf
                    // dem Server darin entstehen, kämen auf dem Mac nicht an.
                    await self.context.store.setBaselineIfUnknown(of: entry.identifier, to: [:])
                    // a folder of that name exists already: adopt it, the system merges both
                    try await finish(entry, rescan: !created)
                    return
                }

                if url == nil && mayAlreadyExist {
                    // found on disk without content: match it with the server copy, or refuse it
                    if let existing = try await client.stat(path: path), !existing.isFolder {
                        try await finish(existing)
                    } else {
                        // No item and no error is the documented answer here
                        // (NSFileProviderReplicatedExtension.h, createItemBasedOnTemplate:
                        // "return a nil item if you are not able to match"): the system
                        // removes the dataless file, which has no content to lose.
                        completionHandler(nil, [], false, nil)
                    }
                    return
                }

                let sha1 = url.flatMap { sha1Hex(of: $0) }
                if mayAlreadyExist, let existing = try await client.stat(path: path, checksums: true) {
                    // the system re-imports what it found on disk
                    if existing.isFolder {
                        completionHandler(nil, [], false, try await collisionError(at: path, client: client))
                        return
                    }
                    if let local = sha1, existing.sha1 == local {
                        try await finish(existing) // the same content, nothing to upload
                        return
                    }
                    // another version is on the server: keep both, the server version stays here
                    if let contents = url {
                        _ = try await uploadConflictCopy(of: contents, named: name, in: parentPath, modified: modified, client: client)
                    }
                    try await finish(existing, fetchContent: true)
                    return
                }

                do {
                    // never replace a file that exists on the server
                    let response = try await client.put(path: path, contents: url, modified: modified, sha1: sha1, ifNoneMatch: "*")
                    try await finish(try await uploadedEntry(at: path, response: response, contents: url, client: client))
                } catch DavError.precondition {
                    // the system renames ours and tries again
                    completionHandler(nil, [], false, try await collisionError(at: path, client: client))
                }
            } catch {
                log.error("Creating \(name, privacy: .private(mask: .hash)) failed: \(logDescription(error), privacy: .public)")
                completionHandler(nil, [], false, WebDavClient.fileProviderError(for: error))
            }
            progress.completedUnitCount = 1
        }
        return progress
    }

    func modifyItem(_ item: NSFileProviderItem,
                    baseVersion version: NSFileProviderItemVersion,
                    changedFields: NSFileProviderItemFields,
                    contents newContents: URL?,
                    options: NSFileProviderModifyItemOptions = [],
                    request: NSFileProviderRequest,
                    completionHandler: @escaping (NSFileProviderItem?, NSFileProviderItemFields, Bool, Error?) -> Void) -> Progress {
        let baseEtag = Self.etag(from: version)
        let identifier = item.itemIdentifier
        let progress = Progress(totalUnitCount: 1)

        if changedFields.contains(.parentItemIdentifier) && item.parentItemIdentifier == .trashContainer {
            Task {
                await self.trash(identifier, baseEtag: baseEtag, completionHandler: completionHandler)
                progress.completedUnitCount = 1
            }
            return progress
        }

        let renamed = changedFields.contains(.filename) || changedFields.contains(.parentItemIdentifier)
        if renamed && SyncRules.isExcluded(item.filename) {
            // Renamed to a name that is never synced (lock/temp files): the item stays on
            // this Mac and the system then calls deleteItem for the server copy
            // (NSFileProviderError.h, excludedFromSync), guarded there by its etag and kept
            // in the server's trash bin - as the desktop client's sync treats such a
            // rename. Refusing it instead would break the save-by-rename of applications.
            completionHandler(nil, [], false, NSFileProviderError(.excludedFromSync))
            return Progress()
        }

        Task {
            do {
                let client = try context.client()
                var current = try await context.entry(for: identifier, client: client)
                // the version the new content builds on; follows the item through a MOVE
                var putBase = baseEtag
                let oldParent = try await context.identifier(ofFolder: current.parentPath, client: client)
                var fetchContent = false

                // Rename / move first; never onto an existing item (412).
                if renamed {
                    let parentPath = try await context.path(for: item.parentItemIdentifier, client: client)
                    let newPath = joinPath(parentPath, item.filename)
                    if newPath != current.path {
                        do {
                            try await client.move(path: current.path, to: newPath)
                        } catch DavError.precondition {
                            completionHandler(nil, [], false, try await collisionError(at: newPath, client: client))
                            return
                        }
                        log.info("Moved \(current.path, privacy: .private(mask: .hash)) -> \(newPath, privacy: .private(mask: .hash))")
                        await context.store.bind(id: identifier, path: newPath)
                        guard let moved = try await client.stat(path: newPath) else { throw DavError.unknownItem }
                        // Some servers give a moved file a new etag. It was the base version
                        // before the MOVE, so it still is: the PUT must not see a conflict.
                        if let base = putBase, base == current.etag, !moved.etag.isEmpty {
                            putBase = moved.etag
                        }
                        current = moved
                    }
                }

                // New content, guarded by the version the user edited: a concurrent
                // edit on the server yields 412 instead of being overwritten.
                if changedFields.contains(.contents), !current.isFolder, let contents = newContents {
                    let modified = item.contentModificationDate ?? nil
                    let localSha1 = sha1Hex(of: contents)
                    do {
                        guard let base = putBase else {
                            throw DavError.precondition // never in sync: no version to build on
                        }
                        let response = try await client.put(path: current.path, contents: contents, modified: modified,
                                                            sha1: localSha1, ifMatch: base)
                        current = try await uploadedEntry(at: current.path, response: response, contents: contents, client: client)
                        log.info("Uploaded \(current.path, privacy: .private(mask: .hash))")
                    } catch DavError.precondition {
                        guard let server = try await client.stat(path: current.path, checksums: true) else {
                            // deleted on the server meanwhile: the system re-creates it from the local version
                            throw DavError.unknownItem
                        }
                        if let local = localSha1, server.sha1 == local {
                            current = server // the server has exactly this content already
                        } else {
                            // Changed on the server meanwhile: keep both. The local version goes
                            // up as a conflict copy, the server version comes down here.
                            log.error("Conflict on \(current.path, privacy: .private(mask: .hash)): keeping the local version as a conflict copy")
                            _ = try await uploadConflictCopy(of: contents, named: current.name, in: current.parentPath,
                                                             modified: modified, client: client)
                            current = server
                            fetchContent = true
                        }
                    }
                }

                let result = try await context.item(for: current, client: client)
                await context.store.noteChild(identifier, seen: current.seen, in: result.parentItemIdentifier, previousContainer: oldParent)
                var handled = Self.syncedFields
                if changedFields.contains(.contents) {
                    handled.insert(.contentModificationDate) // went up with the content (X-OC-Mtime)
                }
                // unsupported fields come back as pending; the system then stops sending them
                completionHandler(result, changedFields.subtracting(handled), fetchContent, nil)
                context.signalWorkingSet()
            } catch {
                log.error("Modifying \(identifier.rawValue, privacy: .private(mask: .hash)) failed: \(logDescription(error), privacy: .public)")
                // noSuchItem (gone on the server): the system removes it, or re-creates
                // it if it has local changes - local edits survive either way
                completionHandler(nil, [], false, WebDavClient.fileProviderError(for: error))
            }
            progress.completedUnitCount = 1
        }
        return progress
    }

    /// Right before a folder goes: has it changed since it was checked (another
    /// client uploaded into it meanwhile)? Then that is not deleted with it. The
    /// server cannot do this check itself: owncloud.online answers a DELETE on a
    /// folder with If-Match always with 412, even for its current etag.
    private func recheckFolder(_ entry: DavEntry, client: WebDavClient) async throws {
        guard let now = try await client.stat(path: entry.path) else {
            throw DavError.http(404) // gone meanwhile: nothing to delete
        }
        if now.identifier != entry.identifier || now.etag != entry.etag {
            throw DavError.precondition
        }
    }

    /// Move to the Trash: delete on the server, where owncloud.online keeps the
    /// item in the user's trash bin (restorable in the web interface), and let
    /// the system remove it from the disk. Something that changed on the server
    /// since the system last saw it is kept instead and handed back where it is.
    private func trash(_ identifier: NSFileProviderItemIdentifier, baseEtag: String?,
                       completionHandler: @escaping (NSFileProviderItem?, NSFileProviderItemFields, Bool, Error?) -> Void) async {
        do {
            let client = try context.client()
            guard let entry = try await context.entryIfExists(for: identifier, client: client) else {
                await context.store.noteRemoval(identifier, from: nil)
                completionHandler(nil, [], false, nil) // already gone
                return
            }
            let parent = try await context.identifier(ofFolder: entry.parentPath, client: client)
            do {
                guard let base = baseEtag, base == entry.etag else { throw DavError.precondition }
                if entry.isFolder {
                    try await recheckFolder(entry, client: client)
                }
                try await client.delete(path: entry.path, ifMatch: entry.isFolder ? nil : base)
            } catch DavError.precondition {
                log.error("Not trashing \(entry.path, privacy: .private(mask: .hash)): it changed on the server")
                let current = try await client.stat(path: entry.path) ?? entry
                context.signalWorkingSet()
                completionHandler(try await context.item(for: current, client: client), [], false, nil)
                return
            }
            log.info("Trashed \(entry.path, privacy: .private(mask: .hash)) (kept in the server's trash bin)")
            await context.store.noteRemoval(identifier, from: parent)
            completionHandler(nil, [], false, nil)
            context.signalWorkingSet()
        } catch DavError.http(404) {
            await context.store.noteRemoval(identifier, from: nil)
            completionHandler(nil, [], false, nil)
        } catch {
            completionHandler(nil, [], false, WebDavClient.fileProviderError(for: error))
        }
    }

    func deleteItem(identifier: NSFileProviderItemIdentifier,
                    baseVersion version: NSFileProviderItemVersion,
                    options: NSFileProviderDeleteItemOptions = [],
                    request: NSFileProviderRequest,
                    completionHandler: @escaping (Error?) -> Void) -> Progress {
        let recursive = options.contains(.recursive)
        let baseEtag = Self.etag(from: version)
        Task {
            do {
                let client = try context.client()
                guard let entry = try await context.entryIfExists(for: identifier, client: client) else {
                    await context.store.noteRemoval(identifier, from: nil)
                    completionHandler(nil) // already gone - deletion is idempotent
                    return
                }
                let parent = try await context.identifier(ofFolder: entry.parentPath, client: client)
                let rejected = { () async throws -> Error in
                    let current = try await client.stat(path: entry.path) ?? entry
                    self.context.signalWorkingSet()
                    return NSError.fileProviderErrorForRejectedDeletion(of: try await self.context.item(for: current, client: client))
                }

                if entry.isFolder {
                    let listing = try await client.propfind(path: entry.path, depth: 1)
                    let hasChildren = listing.contains { $0.path != entry.path && !SyncRules.isExcluded($0.name) }
                    if !recursive && hasChildren {
                        completionHandler(NSFileProviderError(.directoryNotEmpty))
                        return
                    }
                    // A recursive delete takes everything below with it: not if the
                    // folder changed on the server since the system last saw it.
                    if hasChildren && baseEtag != entry.etag {
                        log.error("Not deleting \(entry.path, privacy: .private(mask: .hash)): it changed on the server")
                        completionHandler(try await rejected())
                        return
                    }
                    do {
                        try await recheckFolder(entry, client: client)
                        try await client.delete(path: entry.path)
                    } catch DavError.precondition {
                        log.error("Not deleting \(entry.path, privacy: .private(mask: .hash)): it changed on the server")
                        completionHandler(try await rejected())
                        return
                    }
                } else {
                    // Files: never delete content the user has not seen.
                    guard let base = baseEtag, base == entry.etag else {
                        log.error("Not deleting \(entry.path, privacy: .private(mask: .hash)): it changed on the server")
                        completionHandler(try await rejected())
                        return
                    }
                    do {
                        try await client.delete(path: entry.path, ifMatch: base)
                    } catch DavError.precondition {
                        completionHandler(try await rejected())
                        return
                    }
                }

                await context.store.noteRemoval(identifier, from: parent)
                log.info("Deleted \(entry.path, privacy: .private(mask: .hash))")
                completionHandler(nil)
                context.signalWorkingSet()
            } catch DavError.http(404) {
                await context.store.noteRemoval(identifier, from: nil)
                completionHandler(nil)
            } catch {
                completionHandler(WebDavClient.fileProviderError(for: error))
            }
        }
        return Progress()
    }

    func enumerator(for containerItemIdentifier: NSFileProviderItemIdentifier,
                    request: NSFileProviderRequest) throws -> NSFileProviderEnumerator {
        if containerItemIdentifier != .workingSet && containerItemIdentifier != .trashContainer {
            // the user looks at this folder: bring the working set up to date soon
            context.signalWorkingSet()
        }
        return FileProviderEnumerator(container: containerItemIdentifier, context: context)
    }
}
