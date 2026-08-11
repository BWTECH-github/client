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

/// Enumerates a container's children for the Finder.
///
/// With a configured server this is a live PROPFIND (depth 1); without one it
/// serves the static milestone-1 tree.
final class FileProviderEnumerator: NSObject, NSFileProviderEnumerator {
    private let container: NSFileProviderItemIdentifier
    private let client: WebDavClient?

    init(container: NSFileProviderItemIdentifier, client: WebDavClient?) {
        self.container = container
        self.client = client
        super.init()
    }

    func invalidate() {}

    func enumerateItems(for observer: NSFileProviderEnumerationObserver,
                        startingAt page: NSFileProviderPage) {
        // The working set drives Spotlight/recents; we don't maintain one yet.
        if container == .workingSet {
            observer.finishEnumerating(upTo: nil)
            return
        }

        guard let client = client else {
            if container == .rootContainer {
                observer.didEnumerate(Placeholder.children)
            }
            observer.finishEnumerating(upTo: nil)
            return
        }

        let path = container == .rootContainer ? "" : container.rawValue
        Task {
            do {
                let entries = try await client.propfind(path: path, depth: 1)
                // First entry is the container itself; children follow.
                let children = entries.dropFirst().map(FileProviderItem.init(entry:))
                observer.didEnumerate(Array(children))
                observer.finishEnumerating(upTo: nil)
            } catch {
                observer.finishEnumeratingWithError(WebDavClient.fileProviderError(for: error))
            }
        }
    }

    func enumerateChanges(for observer: NSFileProviderChangeObserver,
                          from anchor: NSFileProviderSyncAnchor) {
        // Change tracking (etag polling + signalEnumerator) lands in milestone 3;
        // until then the tree only refreshes when the system re-enumerates.
        observer.finishEnumeratingChanges(upTo: anchor, moreComing: false)
    }

    func currentSyncAnchor(completionHandler: @escaping (NSFileProviderSyncAnchor?) -> Void) {
        completionHandler(NSFileProviderSyncAnchor(Data("1".utf8)))
    }
}
