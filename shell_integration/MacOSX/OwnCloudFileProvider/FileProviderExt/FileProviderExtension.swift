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
/// configured WebDAV server (milestone 2) it enumerates and hydrates real
/// server data read-only; mutations arrive with milestone 3.
final class FileProviderExtension: NSObject, NSFileProviderReplicatedExtension {
    private let domain: NSFileProviderDomain
    private let client: WebDavClient?
    private let log = Logger(subsystem: "online.owncloud.desktopclient", category: "fileprovider")

    required init(domain: NSFileProviderDomain) {
        self.domain = domain
        if let config = DavConfig.load() {
            self.client = WebDavClient(config: config)
        } else {
            self.client = nil
        }
        super.init()
        log.info("Extension started for domain \(domain.identifier.rawValue, privacy: .public), server configured: \(self.client != nil)")
    }

    func invalidate() {
        log.info("Extension invalidated for domain \(self.domain.identifier.rawValue, privacy: .public)")
    }

    func item(for identifier: NSFileProviderItemIdentifier,
              request: NSFileProviderRequest,
              completionHandler: @escaping (NSFileProviderItem?, Error?) -> Void) -> Progress {
        guard let client = client else {
            if let item = Placeholder.byIdentifier[identifier] {
                completionHandler(item, nil)
            } else {
                completionHandler(nil, NSFileProviderError(.noSuchItem))
            }
            return Progress()
        }

        if identifier == .trashContainer {
            completionHandler(nil, NSFileProviderError(.noSuchItem))
            return Progress()
        }

        let path = identifier == .rootContainer ? "" : identifier.rawValue
        Task {
            do {
                let entries = try await client.propfind(path: path, depth: 0)
                guard let entry = entries.first else {
                    completionHandler(nil, NSFileProviderError(.noSuchItem))
                    return
                }
                completionHandler(FileProviderItem(entry: entry), nil)
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
        guard let client = client else {
            completionHandler(nil, nil, NSError(domain: NSCocoaErrorDomain, code: NSFeatureUnsupportedError))
            return Progress()
        }

        let path = itemIdentifier.rawValue
        let progress = Progress(totalUnitCount: 1)
        Task {
            do {
                // Fresh metadata first so the returned item matches the bytes.
                guard let entry = try await client.propfind(path: path, depth: 0).first, !entry.isFolder else {
                    completionHandler(nil, nil, NSFileProviderError(.noSuchItem))
                    return
                }
                let url = try await client.fetch(path: path)
                progress.completedUnitCount = 1
                completionHandler(url, FileProviderItem(entry: entry), nil)
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
        completionHandler(nil, [], false, NSError(domain: NSCocoaErrorDomain, code: NSFeatureUnsupportedError))
        return Progress()
    }

    func modifyItem(_ item: NSFileProviderItem,
                    baseVersion version: NSFileProviderItemVersion,
                    changedFields: NSFileProviderItemFields,
                    contents newContents: URL?,
                    options: NSFileProviderModifyItemOptions = [],
                    request: NSFileProviderRequest,
                    completionHandler: @escaping (NSFileProviderItem?, NSFileProviderItemFields, Bool, Error?) -> Void) -> Progress {
        completionHandler(nil, [], false, NSError(domain: NSCocoaErrorDomain, code: NSFeatureUnsupportedError))
        return Progress()
    }

    func deleteItem(identifier: NSFileProviderItemIdentifier,
                    baseVersion version: NSFileProviderItemVersion,
                    options: NSFileProviderDeleteItemOptions = [],
                    request: NSFileProviderRequest,
                    completionHandler: @escaping (Error?) -> Void) -> Progress {
        completionHandler(NSError(domain: NSCocoaErrorDomain, code: NSFeatureUnsupportedError))
        return Progress()
    }

    func enumerator(for containerItemIdentifier: NSFileProviderItemIdentifier,
                    request: NSFileProviderRequest) throws -> NSFileProviderEnumerator {
        FileProviderEnumerator(container: containerItemIdentifier, client: client)
    }
}
