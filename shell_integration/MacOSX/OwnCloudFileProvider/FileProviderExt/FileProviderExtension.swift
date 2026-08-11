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

/// The owncloud.online File Provider extension.
///
/// This is the process the Finder talks to: it owns one domain (one synced
/// folder) and answers metadata, enumeration and content requests. With a
/// configured WebDAV server it serves the live server tree: enumeration and
/// hydration (milestone 2) plus create/upload/rename/move/delete with
/// conditional writes to avoid lost updates (milestone 3).
final class FileProviderExtension: NSObject, NSFileProviderReplicatedExtension {
    private let domain: NSFileProviderDomain
    private let client: WebDavClient?
    private let store: ItemStore?
    private let log = Logger(subsystem: "online.owncloud.desktopclient", category: "fileprovider")

    required init(domain: NSFileProviderDomain) {
        self.domain = domain
        let loaded = DavConfig.load(domainIdentifier: domain.identifier.rawValue)
        if let config = loaded, config.isTransportAcceptable {
            self.client = WebDavClient(config: config)
            self.store = ItemStore(domainIdentifier: domain.identifier.rawValue)
        } else {
            if let config = loaded, !config.isTransportAcceptable {
                Logger(subsystem: "online.owncloud.desktopclient", category: "fileprovider")
                    .error("Refusing plain-http WebDAV for a non-local host; use https")
            }
            self.client = nil
            self.store = nil
        }
        super.init()
        log.info("Extension started for domain \(domain.identifier.rawValue, privacy: .public), server configured: \(self.client != nil)")
    }

    func invalidate() {
        log.info("Extension invalidated for domain \(self.domain.identifier.rawValue, privacy: .public)")
    }

    // MARK: - Helpers

    private static func etag(from version: NSFileProviderItemVersion?) -> String? {
        guard let data = version?.contentVersion, !data.isEmpty else { return nil }
        return String(data: data, encoding: .utf8)
    }

    /// Resolve an identifier to a server path, recovering the mapping from the
    /// server (fileid -> path) if our local store lost it (e.g. after a store
    /// reset), so items never become permanently orphaned.
    private func path(for identifier: NSFileProviderItemIdentifier,
                      client: WebDavClient, store: ItemStore) async throws -> String {
        if let path = await store.path(for: identifier) { return path }
        if let recovered = try await client.resolvePath(fileId: identifier.rawValue) {
            await store.bind(id: identifier, path: recovered)
            return recovered
        }
        throw DavError.unknownItem
    }

    /// Fetch fresh metadata for an identifier and build the item.
    private func item(forPath path: String, client: WebDavClient, store: ItemStore) async throws -> FileProviderItem {
        guard let entry = try await client.propfind(path: path, depth: 0).first else {
            throw DavError.http(404)
        }
        await store.register(entries: [entry])
        let parent = await store.identifier(forPath: entry.parentPath) ?? .rootContainer
        return FileProviderItem(entry: entry, parent: parent)
    }

    // MARK: - NSFileProviderReplicatedExtension

    func item(for identifier: NSFileProviderItemIdentifier,
              request: NSFileProviderRequest,
              completionHandler: @escaping (NSFileProviderItem?, Error?) -> Void) -> Progress {
        guard let client = client, let store = store else {
            if let item = Placeholder.byIdentifier[identifier] {
                completionHandler(item, nil)
            } else {
                completionHandler(nil, NSFileProviderError(.noSuchItem))
            }
            return Progress()
        }

        if identifier == .trashContainer || identifier == .workingSet {
            completionHandler(nil, NSFileProviderError(.noSuchItem))
            return Progress()
        }

        Task {
            do {
                let path = try await self.path(for: identifier, client: client, store: store)
                completionHandler(try await self.item(forPath: path, client: client, store: store), nil)
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
        guard let client = client, let store = store else {
            completionHandler(nil, nil, NSError(domain: NSCocoaErrorDomain, code: NSFeatureUnsupportedError))
            return Progress()
        }

        let progress = Progress(totalUnitCount: 100)
        Task {
            do {
                let path = try await self.path(for: itemIdentifier, client: client, store: store)
                guard let entry = try await client.propfind(path: path, depth: 0).first, !entry.isFolder else {
                    completionHandler(nil, nil, NSFileProviderError(.noSuchItem))
                    return
                }
                let url = try await client.fetch(path: entry.path, progress: progress)
                let parent = await store.identifier(forPath: entry.parentPath) ?? .rootContainer
                completionHandler(url, FileProviderItem(entry: entry, parent: parent), nil)
            } catch {
                completionHandler(nil, nil, WebDavClient.fileProviderError(for: error))
            }
        }
        return progress
    }

    func createItem(basedOn itemTemplate: NSFileProviderItem,
                    fields: NSFileProviderItemFields,
                    contents url: URL?,
                    options: NSFileProviderCreateItemOptions = [],
                    request: NSFileProviderRequest,
                    completionHandler: @escaping (NSFileProviderItem?, NSFileProviderItemFields, Bool, Error?) -> Void) -> Progress {
        guard let client = client, let store = store else {
            completionHandler(nil, [], false, NSError(domain: NSCocoaErrorDomain, code: NSFeatureUnsupportedError))
            return Progress()
        }

        let mayAlreadyExist = options.contains(.mayAlreadyExist)
        Task {
            do {
                guard let parentPath = await store.path(for: itemTemplate.parentItemIdentifier) else {
                    throw DavError.unknownItem
                }
                let name = itemTemplate.filename
                let path = parentPath.isEmpty ? name : "\(parentPath)/\(name)"
                let isFolder = itemTemplate.contentType == .folder || (itemTemplate.contentType?.conforms(to: .directory) ?? false)

                if isFolder {
                    // Binds to the folder if it already exists (mkcol returns false).
                    _ = try await client.mkcol(path: path)
                } else {
                    do {
                        // Refuse to clobber an existing server file on a fresh
                        // create, unless the system is re-importing a known item.
                        try await client.put(path: path, contents: url,
                                             ifNoneMatch: mayAlreadyExist ? nil : "*")
                    } catch DavError.precondition {
                        if !mayAlreadyExist {
                            // A different file already exists under this name.
                            completionHandler(nil, [], false, NSFileProviderError(.filenameCollision))
                            return
                        }
                        // Re-import of a file we already have: adopt the server copy.
                    }
                }

                let item = try await self.item(forPath: path, client: client, store: store)
                self.log.info("Created \(isFolder ? "folder" : "file", privacy: .public) \(path, privacy: .public)")
                completionHandler(item, [], false, nil)
            } catch {
                completionHandler(nil, [], false, WebDavClient.fileProviderError(for: error))
            }
        }
        return Progress()
    }

    func modifyItem(_ item: NSFileProviderItem,
                    baseVersion version: NSFileProviderItemVersion,
                    changedFields: NSFileProviderItemFields,
                    contents newContents: URL?,
                    options: NSFileProviderModifyItemOptions = [],
                    request: NSFileProviderRequest,
                    completionHandler: @escaping (NSFileProviderItem?, NSFileProviderItemFields, Bool, Error?) -> Void) -> Progress {
        guard let client = client, let store = store else {
            completionHandler(nil, [], false, NSError(domain: NSCocoaErrorDomain, code: NSFeatureUnsupportedError))
            return Progress()
        }

        // We do not sync a trash: a move into the trash falls back to a delete.
        if changedFields.contains(.parentItemIdentifier) && item.parentItemIdentifier == .trashContainer {
            completionHandler(nil, [], false, NSError(domain: NSCocoaErrorDomain, code: NSFeatureUnsupportedError))
            return Progress()
        }

        let baseEtag = Self.etag(from: version)
        Task {
            do {
                let oldPath = try await self.path(for: item.itemIdentifier, client: client, store: store)
                var path = oldPath

                // Rename / move first (fails with 412 if the destination exists).
                if changedFields.contains(.filename) || changedFields.contains(.parentItemIdentifier) {
                    guard let parentPath = await store.path(for: item.parentItemIdentifier) else {
                        throw DavError.unknownItem
                    }
                    let newPath = parentPath.isEmpty ? item.filename : "\(parentPath)/\(item.filename)"
                    if newPath != oldPath {
                        do {
                            try await client.move(path: oldPath, to: newPath)
                        } catch DavError.precondition {
                            completionHandler(nil, [], false, NSFileProviderError(.filenameCollision))
                            return
                        }
                        await store.move(id: item.itemIdentifier, to: newPath)
                        self.log.info("Moved \(oldPath, privacy: .public) -> \(newPath, privacy: .public)")
                        path = newPath
                    }
                }

                // Upload new content guarded by the base etag: a concurrent
                // remote edit yields 412 instead of a silent overwrite.
                if changedFields.contains(.contents) {
                    do {
                        try await client.put(path: path, contents: newContents, ifMatch: baseEtag)
                        self.log.info("Uploaded \(path, privacy: .public)")
                    } catch DavError.precondition {
                        // Server content is newer. Do not overwrite it; hand the
                        // current server item back so the system reconciles (the
                        // user's edit surfaces as an unsynced change / conflict).
                        self.log.error("Conflict on upload of \(path, privacy: .public); keeping server copy")
                        let current = try await self.item(forPath: path, client: client, store: store)
                        completionHandler(current, [], false, nil)
                        return
                    }
                }

                completionHandler(try await self.item(forPath: path, client: client, store: store), [], false, nil)
            } catch {
                completionHandler(nil, [], false, WebDavClient.fileProviderError(for: error))
            }
        }
        return Progress()
    }

    func deleteItem(identifier: NSFileProviderItemIdentifier,
                    baseVersion version: NSFileProviderItemVersion,
                    options: NSFileProviderDeleteItemOptions = [],
                    request: NSFileProviderRequest,
                    completionHandler: @escaping (Error?) -> Void) -> Progress {
        guard let client = client, let store = store else {
            completionHandler(NSError(domain: NSCocoaErrorDomain, code: NSFeatureUnsupportedError))
            return Progress()
        }

        let recursive = options.contains(.recursive)
        Task {
            do {
                guard let path = await store.path(for: identifier) else {
                    completionHandler(nil) // already gone - deletion is idempotent
                    return
                }

                // Learn the item's kind (and, for a folder, its children) with a
                // single depth-1 request.
                let listing = try await client.propfind(path: path, depth: 1)
                let isFolder = listing.first { $0.identifier == identifier }?.isFolder ?? false

                if isFolder {
                    // Guard against destroying unsynced server-side children: if
                    // the caller did not ask for a recursive delete, refuse a
                    // non-empty folder instead of nuking whatever the server holds.
                    if !recursive && listing.count > 1 {
                        completionHandler(NSFileProviderError(.directoryNotEmpty))
                        return
                    }
                    // A folder's etag changes when its children change, so an
                    // If-Match would spuriously fail; the emptiness check above
                    // is the safety net instead.
                    try await client.delete(path: path)
                } else {
                    // Files: guard against deleting content a concurrent edit
                    // just changed.
                    try await client.delete(path: path, ifMatch: Self.etag(from: version))
                }

                await store.remove(id: identifier)
                self.log.info("Deleted \(path, privacy: .public)")
                completionHandler(nil)
            } catch DavError.precondition {
                // Changed on the server since the delete was queued: let the
                // system re-sync rather than deleting the newer content.
                completionHandler(NSFileProviderError(.serverUnreachable))
            } catch DavError.http(404) {
                await store.remove(id: identifier)
                completionHandler(nil)
            } catch {
                completionHandler(WebDavClient.fileProviderError(for: error))
            }
        }
        return Progress()
    }

    func enumerator(for containerItemIdentifier: NSFileProviderItemIdentifier,
                    request: NSFileProviderRequest) throws -> NSFileProviderEnumerator {
        FileProviderEnumerator(container: containerItemIdentifier, client: client, store: store)
    }
}
