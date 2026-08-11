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
/// serves the static milestone-1 tree. Change enumeration diffs the current
/// listing against the etag/path snapshot from the previous enumeration and,
/// crucially, only commits that snapshot after the whole walk succeeds - a
/// mid-walk failure is retried from the same anchor without losing changes.
final class FileProviderEnumerator: NSObject, NSFileProviderEnumerator {
    private let container: NSFileProviderItemIdentifier
    private let client: WebDavClient?
    private let store: ItemStore?

    init(container: NSFileProviderItemIdentifier, client: WebDavClient?, store: ItemStore?) {
        self.container = container
        self.client = client
        self.store = store
        super.init()
    }

    func invalidate() {}

    /// The children of a depth-1 listing: every entry except the container
    /// itself (identified by matching identifier, not by position).
    private func children(of entries: [DavEntry], container: NSFileProviderItemIdentifier) -> [DavEntry] {
        entries.filter { $0.identifier != container }
    }

    func enumerateItems(for observer: NSFileProviderEnumerationObserver,
                        startingAt page: NSFileProviderPage) {
        if container == .workingSet {
            observer.finishEnumerating(upTo: nil)
            return
        }

        guard let client = client, let store = store else {
            if container == .rootContainer {
                observer.didEnumerate(Placeholder.children)
            }
            observer.finishEnumerating(upTo: nil)
            return
        }

        let container = self.container
        Task {
            do {
                guard let path = await store.path(for: container) else {
                    observer.finishEnumeratingWithError(NSFileProviderError(.noSuchItem))
                    return
                }
                let entries = try await client.propfind(path: path, depth: 1)
                let children = self.children(of: entries, container: container)
                await store.register(entries: children)
                await store.setChildren(of: container, to: children.etagSnapshot())
                observer.didEnumerate(children.map { FileProviderItem(entry: $0, parent: container) })
                observer.finishEnumerating(upTo: nil)
            } catch {
                observer.finishEnumeratingWithError(WebDavClient.fileProviderError(for: error))
            }
        }
    }

    func enumerateChanges(for observer: NSFileProviderChangeObserver,
                          from anchor: NSFileProviderSyncAnchor) {
        guard let client = client, let store = store else {
            observer.finishEnumeratingChanges(upTo: anchor, moreComing: false)
            return
        }

        let container = self.container
        Task {
            // A lost delta baseline (corrupt store) means we cannot answer
            // "changes since anchor N"; ask the system to re-enumerate instead.
            if await store.didReset {
                observer.finishEnumeratingWithError(NSFileProviderError(.syncAnchorExpired))
                return
            }

            do {
                let containers: [NSFileProviderItemIdentifier]
                if container == .workingSet {
                    containers = await store.knownContainers()
                } else {
                    containers = [container]
                }

                // Collect everything first; commit to the store only after the
                // whole walk succeeds, so a failure part-way is safely retried.
                var updates: [FileProviderItem] = []
                var deletions: [NSFileProviderItemIdentifier] = []
                var pendingSnapshots: [(NSFileProviderItemIdentifier, [String: String])] = []
                var pendingBindings: [(NSFileProviderItemIdentifier, String)] = []
                var pendingRemovals: [NSFileProviderItemIdentifier] = []

                for current in containers {
                    guard let path = await store.path(for: current) else { continue }
                    let entries: [DavEntry]
                    do {
                        entries = try await client.propfind(path: path, depth: 1)
                    } catch DavError.http(404) {
                        continue // container gone; its parent's diff reports it
                    }
                    let children = self.children(of: entries, container: current)
                    let snapshot = children.etagSnapshot()
                    let previous = await store.children(of: current)

                    for entry in children {
                        let id = entry.identifier.rawValue
                        let etagChanged = previous[id] != entry.etag
                        let pathChanged = await store.path(for: entry.identifier) != entry.path
                        if etagChanged || pathChanged {
                            updates.append(FileProviderItem(entry: entry, parent: current))
                            pendingBindings.append((entry.identifier, entry.path))
                        }
                    }

                    // Ids that vanished from this container: distinguish a real
                    // deletion from a server-side move (same fileid, new path).
                    for goneId in previous.keys where snapshot[goneId] == nil {
                        let ident = NSFileProviderItemIdentifier(goneId)
                        if let movedPath = try await client.resolvePath(fileId: goneId) {
                            pendingBindings.append((ident, movedPath))
                            if let moved = try await client.propfind(path: movedPath, depth: 0).first {
                                let parent = await store.identifier(forPath: moved.parentPath) ?? .rootContainer
                                updates.append(FileProviderItem(entry: moved, parent: parent))
                            }
                        } else {
                            deletions.append(ident)
                            pendingRemovals.append(ident)
                        }
                    }

                    pendingSnapshots.append((current, snapshot))
                }

                // Commit.
                for (id, path) in pendingBindings { await store.bind(id: id, path: path) }
                for (container, snapshot) in pendingSnapshots { await store.setChildren(of: container, to: snapshot) }
                for id in pendingRemovals { await store.remove(id: id) }

                if !updates.isEmpty { observer.didUpdate(updates) }
                if !deletions.isEmpty { observer.didDeleteItems(withIdentifiers: deletions) }

                let changed = !updates.isEmpty || !deletions.isEmpty
                let anchorValue = changed ? await store.bumpAnchor() : await store.currentAnchor()
                observer.finishEnumeratingChanges(upTo: NSFileProviderSyncAnchor(Data("\(anchorValue)".utf8)),
                                                  moreComing: false)
            } catch {
                observer.finishEnumeratingWithError(WebDavClient.fileProviderError(for: error))
            }
        }
    }

    func currentSyncAnchor(completionHandler: @escaping (NSFileProviderSyncAnchor?) -> Void) {
        guard let store = store else {
            completionHandler(NSFileProviderSyncAnchor(Data("1".utf8)))
            return
        }
        Task {
            let anchor = await store.currentAnchor()
            completionHandler(NSFileProviderSyncAnchor(Data("\(anchor)".utf8)))
        }
    }
}
