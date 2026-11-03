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
    let entry: DavEntry
    private let parentIdentifier: NSFileProviderItemIdentifier

    init(entry: DavEntry, parent: NSFileProviderItemIdentifier) {
        self.entry = entry
        self.parentIdentifier = entry.path.isEmpty ? .rootContainer : parent
    }

    var itemIdentifier: NSFileProviderItemIdentifier { entry.identifier }
    var parentItemIdentifier: NSFileProviderItemIdentifier { parentIdentifier }
    // The root's name is not shown (the system uses the domain's display name).
    var filename: String { entry.path.isEmpty ? "owncloud.online" : entry.name }

    var contentType: UTType {
        if entry.isFolder {
            return .folder
        }
        let ext = (entry.name as NSString).pathExtension
        if ext.isEmpty {
            return .data
        }
        return UTType(filenameExtension: ext) ?? .data
    }

    /// oc:permissions: W write, C/K create file/folder, N rename, V move,
    /// D delete. Without permissions from the server everything is allowed and
    /// the server has the last word.
    private func permitted(_ letters: String) -> Bool {
        entry.permissions.isEmpty || letters.contains(where: { entry.permissions.contains($0) })
    }

    var capabilities: NSFileProviderItemCapabilities {
        var caps: NSFileProviderItemCapabilities = [.allowsReading]
        if entry.isFolder {
            caps.insert(.allowsContentEnumerating)
            if permitted("CK") { caps.insert(.allowsAddingSubItems) }
        } else if permitted("W") {
            caps.insert(.allowsWriting)
        }
        if entry.path.isEmpty {
            return caps // the domain root can neither be renamed, moved nor deleted
        }
        if permitted("N") { caps.insert(.allowsRenaming) }
        if permitted("V") { caps.insert(.allowsReparenting) }
        if permitted("D") {
            // Moving to the Trash deletes on the server, where owncloud.online keeps
            // the item in the user's trash bin (see modifyItem).
            caps.insert(.allowsDeleting)
            caps.insert(.allowsTrashing)
        }
        return caps
    }

    var fileSystemFlags: NSFileProviderFileSystemFlags {
        var flags: NSFileProviderFileSystemFlags = [.userReadable]
        if entry.isFolder {
            flags.insert(.userExecutable)
            if permitted("CK") { flags.insert(.userWritable) }
        } else if permitted("W") {
            flags.insert(.userWritable)
        }
        return flags
    }

    var documentSize: NSNumber? { entry.isFolder ? nil : NSNumber(value: entry.size) }
    var contentModificationDate: Date? { entry.modified }

    var itemVersion: NSFileProviderItemVersion {
        let etag = entry.etag.isEmpty ? "unknown" : entry.etag
        // The etag stays the same on a rename on some servers; the path makes
        // sure a metadata change is always a new metadata version.
        return NSFileProviderItemVersion(contentVersion: Data(etag.utf8),
                                         metadataVersion: Data("\(etag)|\(entry.path)".utf8))
    }
}
