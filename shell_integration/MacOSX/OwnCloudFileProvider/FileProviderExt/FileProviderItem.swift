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

/// An item exposed to the Finder.
///
/// Milestone 1 serves a small static tree so the whole pipeline (domain ->
/// extension -> Finder) can be verified without any network access. The WebDAV
/// backed model replaces `Placeholder.tree` in milestone 2; everything else in
/// this class stays as is.
final class FileProviderItem: NSObject, NSFileProviderItem {
    let identifier: NSFileProviderItemIdentifier
    let parentIdentifier: NSFileProviderItemIdentifier
    let name: String
    let isFolder: Bool
    let size: Int64

    init(identifier: NSFileProviderItemIdentifier,
         parent: NSFileProviderItemIdentifier,
         name: String,
         isFolder: Bool,
         size: Int64 = 0) {
        self.identifier = identifier
        self.parentIdentifier = parent
        self.name = name
        self.isFolder = isFolder
        self.size = size
    }

    var itemIdentifier: NSFileProviderItemIdentifier { identifier }
    var parentItemIdentifier: NSFileProviderItemIdentifier { parentIdentifier }
    var filename: String { name }
    var contentType: UTType { isFolder ? .folder : .data }

    var capabilities: NSFileProviderItemCapabilities {
        isFolder ? [.allowsReading, .allowsContentEnumerating] : [.allowsReading]
    }

    var documentSize: NSNumber? { isFolder ? nil : NSNumber(value: size) }

    var itemVersion: NSFileProviderItemVersion {
        NSFileProviderItemVersion(contentVersion: Data("1".utf8),
                                  metadataVersion: Data("1".utf8))
    }
}

/// The static milestone-1 tree.
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
