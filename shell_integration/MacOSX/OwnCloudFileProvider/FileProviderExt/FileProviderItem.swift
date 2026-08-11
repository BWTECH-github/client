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
import UniformTypeIdentifiers

/// An item exposed to the Finder, backed by a WebDAV PROPFIND entry.
///
/// Identifiers are the server's stable file ids (oc:fileid), so they survive
/// renames and moves the way the File Provider contract requires.
final class FileProviderItem: NSObject, NSFileProviderItem {
    let identifier: NSFileProviderItemIdentifier
    let parentIdentifier: NSFileProviderItemIdentifier
    let name: String
    let isFolder: Bool
    let size: Int64
    let etag: String
    let modified: Date?

    init(identifier: NSFileProviderItemIdentifier,
         parent: NSFileProviderItemIdentifier,
         name: String,
         isFolder: Bool,
         size: Int64 = 0,
         etag: String = "static",
         modified: Date? = nil) {
        self.identifier = identifier
        self.parentIdentifier = parent
        self.name = name
        self.isFolder = isFolder
        self.size = size
        self.etag = etag
        self.modified = modified
    }

    convenience init(entry: DavEntry, parent: NSFileProviderItemIdentifier) {
        self.init(identifier: entry.identifier,
                  parent: parent,
                  name: entry.path.isEmpty ? "owncloud.online" : entry.name,
                  isFolder: entry.isFolder,
                  size: entry.size,
                  etag: entry.etag.isEmpty ? "unknown" : entry.etag,
                  modified: entry.modified)
    }

    var itemIdentifier: NSFileProviderItemIdentifier { identifier }
    var parentItemIdentifier: NSFileProviderItemIdentifier { parentIdentifier }
    var filename: String { name }

    var contentType: UTType {
        if isFolder {
            return .folder
        }
        let ext = (name as NSString).pathExtension
        if ext.isEmpty {
            return .data
        }
        return UTType(filenameExtension: ext) ?? .data
    }

    var capabilities: NSFileProviderItemCapabilities {
        if isFolder {
            return [.allowsReading, .allowsContentEnumerating, .allowsAddingSubItems,
                    .allowsRenaming, .allowsReparenting, .allowsDeleting]
        }
        return [.allowsReading, .allowsWriting, .allowsRenaming, .allowsReparenting, .allowsDeleting]
    }

    var documentSize: NSNumber? { isFolder ? nil : NSNumber(value: size) }
    var contentModificationDate: Date? { modified }

    var itemVersion: NSFileProviderItemVersion {
        NSFileProviderItemVersion(contentVersion: Data(etag.utf8),
                                  metadataVersion: Data(etag.utf8))
    }
}

/// Static fallback tree, served when no server configuration exists yet. It
/// keeps the extension verifiable (milestone 1 behaviour) on machines where
/// the client has not written `FileProviderConfig.json`.
enum Placeholder {
    static let root = FileProviderItem(identifier: .rootContainer,
                                       parent: .rootContainer,
                                       name: "owncloud.online",
                                       isFolder: true)

    static let children: [FileProviderItem] = [
        FileProviderItem(identifier: .init("documents"), parent: .rootContainer,
                         name: "Documents", isFolder: true),
        FileProviderItem(identifier: .init("photos"), parent: .rootContainer,
                         name: "Photos", isFolder: true),
        FileProviderItem(identifier: .init("readme.txt"), parent: .rootContainer,
                         name: "README.txt", isFolder: false, size: 64),
    ]

    static let byIdentifier: [NSFileProviderItemIdentifier: FileProviderItem] = {
        var map: [NSFileProviderItemIdentifier: FileProviderItem] = [.rootContainer: root]
        for child in children {
            map[child.identifier] = child
        }
        return map
    }()
}
