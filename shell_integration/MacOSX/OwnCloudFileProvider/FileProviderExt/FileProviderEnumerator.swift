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

/// Enumerates a container's children for the Finder (a live PROPFIND, depth 1),
/// and the changes of the working set.
///
/// Replicated extensions report every remote change through the working set:
/// the client signals it when the folder's etag on the server changed, the
/// extension after its own writes. Change enumeration walks the known folders
/// (DomainContext.walkChanges) and stages the result in the ItemStore until
/// the system has taken it (two-phase anchors), so nothing is lost when an
/// enumeration is interrupted.
final class FileProviderEnumerator: NSObject, NSFileProviderEnumerator {
    private let container: NSFileProviderItemIdentifier
    private let context: DomainContext
    /// The listing of the container, kept for the following pages.
    private var listing: [FileProviderItem]?
    private let log = Logger(subsystem: logSubsystem, category: "enumeration")

    init(container: NSFileProviderItemIdentifier, context: DomainContext) {
        self.container = container
        self.context = context
        super.init()
    }

    func invalidate() {
        listing = nil
    }

    // MARK: pages

    private static let pagePrefix = "offset:"

    private static func offset(of page: NSFileProviderPage) -> Int {
        guard let text = String(data: page.rawValue, encoding: .utf8), text.hasPrefix(pagePrefix) else { return 0 }
        return Int(text.dropFirst(pagePrefix.count)) ?? 0
    }

    private static func page(at offset: Int) -> NSFileProviderPage {
        NSFileProviderPage(Data("\(pagePrefix)\(offset)".utf8))
    }

    private static func anchor(_ value: Int) -> NSFileProviderSyncAnchor {
        NSFileProviderSyncAnchor(Data("\(value)".utf8))
    }

    private static func value(of anchor: NSFileProviderSyncAnchor) -> Int? {
        String(data: anchor.rawValue, encoding: .utf8).flatMap { Int($0) }
    }

    // MARK: NSFileProviderEnumerator

    func enumerateItems(for observer: NSFileProviderEnumerationObserver, startingAt page: NSFileProviderPage) {
        if container == .trashContainer {
            // Moving to the Trash deletes on the server, so our trash stays empty.
            observer.finishEnumerating(upTo: nil)
            return
        }
        let offset = Self.offset(of: page)
        let pageSize = max(observer.suggestedPageSize ?? 200, 100)
        Task {
            do {
                if offset == 0 || self.listing == nil {
                    self.listing = try await self.load()
                }
                let items = self.listing ?? []
                let start = min(offset, items.count)
                let end = min(start + pageSize, items.count)
                if start < end {
                    observer.didEnumerate(Array(items[start..<end]))
                }
                if end < items.count {
                    observer.finishEnumerating(upTo: Self.page(at: end))
                } else {
                    self.listing = nil
                    observer.finishEnumerating(upTo: nil)
                }
            } catch {
                self.listing = nil
                self.log.error("Enumerating \(self.container.rawValue, privacy: .private(mask: .hash)) failed: \(logDescription(error), privacy: .public)")
                observer.finishEnumeratingWithError(WebDavClient.fileProviderError(for: error))
            }
        }
    }

    /// The whole listing of the container, as items.
    private func load() async throws -> [FileProviderItem] {
        let client = try context.client()
        if container == .workingSet {
            // A full enumeration of the working set (new domain, expired anchor):
            // everything in the folders the system knows, the root always (also its
            // baseline: change enumeration starts there). The baselines that exist
            // stay, so the next change enumeration still reports what changed since.
            // After a lost store (needsRebuild) nothing is known, while the system
            // still has items on disk: then the whole tree, so none of them looks
            // deleted and change enumeration covers every folder again.
            let store = context.store
            let rebuild = await store.needsRebuild
            var queue = await store.knownContainers()
            if !queue.contains(where: { $0.0 == .rootContainer }) {
                queue.insert((.rootContainer, ""), at: 0)
            }
            var visited = Set<String>()
            var items: [FileProviderItem] = []
            while !queue.isEmpty {
                let (known, path) = queue.removeFirst()
                guard visited.insert(known.rawValue).inserted else { continue }
                let entries: [DavEntry]
                do {
                    entries = try await client.propfind(path: path, depth: 1)
                } catch DavError.http(404) {
                    continue
                }
                let children = entries.filter { $0.path != path && !SyncRules.isExcluded($0.name) }
                await store.register(entries: children)
                let pins = await store.pins()
                var snapshot: [String: Seen] = [:]
                for child in children {
                    snapshot[child.identifier.rawValue] = child.seen
                    if rebuild && child.isFolder {
                        queue.append((child.identifier, child.path))
                    }
                }
                await store.setBaselineIfUnknown(of: known, to: snapshot)
                items.append(contentsOf: children.map { FileProviderItem(entry: $0, parent: known, pin: pins[$0.identifier.rawValue]) })
            }
            if rebuild {
                await store.finishRebuild()
            }
            return items
        }

        let path = try await context.path(for: container, client: client)
        let entries = try await client.propfind(path: path, depth: 1)
        guard let own = entries.first(where: { $0.path == path }) else {
            throw DavError.unknownItem
        }
        if !own.isFolder {
            // a document enumerator (live updates for an open file): the item itself
            return [try await context.item(for: own, client: client)]
        }
        let children = entries.filter { $0.path != path && !SyncRules.isExcluded($0.name) }
        await context.store.register(entries: children)
        var snapshot: [String: Seen] = [:]
        for child in children {
            snapshot[child.identifier.rawValue] = child.seen
        }
        await context.store.setBaselineIfUnknown(of: container, to: snapshot)
        let pins = await context.store.pins()
        return children.map { FileProviderItem(entry: $0, parent: container, pin: pins[$0.identifier.rawValue]) }
    }

    func enumerateChanges(for observer: NSFileProviderChangeObserver, from anchor: NSFileProviderSyncAnchor) {
        guard container == .workingSet else {
            // All remote changes arrive through the working set.
            observer.finishEnumeratingChanges(upTo: anchor, moreComing: false)
            return
        }
        let batchSize = max(observer.suggestedBatchSize ?? 200, 100)
        Task {
            let store = self.context.store
            // A lost baseline (corrupt store) cannot answer "changes since N":
            // the system re-enumerates instead.
            if await store.needsFullReport {
                await store.clearFullReport()
                observer.finishEnumeratingWithError(NSFileProviderError(.syncAnchorExpired))
                return
            }
            if await store.didReset {
                await store.clearReset()
                observer.finishEnumeratingWithError(NSFileProviderError(.syncAnchorExpired))
                return
            }
            guard let value = Self.value(of: anchor), await store.beginChanges(from: value) else {
                self.log.info("Unknown sync anchor, asking the system to re-enumerate")
                observer.finishEnumeratingWithError(NSFileProviderError(.syncAnchorExpired))
                return
            }
            do {
                var changes = try await self.context.walkChanges()
                let client = try self.context.client()
                let toEvict = await self.context.takePinRequests(client: client)
                let pinned = try await self.context.pinUpdates(client: client)
                let walked = Set(changes.updates.map(\.itemIdentifier))
                changes.updates.append(contentsOf: pinned.items.filter { !walked.contains($0.itemIdentifier) })
                if changes.isEmpty {
                    observer.finishEnumeratingChanges(upTo: Self.anchor(await store.currentAnchor()), moreComing: false)
                    self.context.evict(toEvict)
                    return
                }
                let next = await store.stage(snapshots: changes.snapshots, removedContainers: changes.removedContainers,
                                             pinsReported: pinned.ids)
                self.log.info("Reporting \(changes.updates.count) updated and \(changes.deletions.count) deleted items")
                var start = 0
                while start < changes.updates.count {
                    let end = min(start + batchSize, changes.updates.count)
                    observer.didUpdate(Array(changes.updates[start..<end]))
                    start = end
                }
                if !changes.deletions.isEmpty {
                    observer.didDeleteItems(withIdentifiers: changes.deletions)
                }
                observer.finishEnumeratingChanges(upTo: Self.anchor(next), moreComing: false)
                self.context.evict(toEvict)
            } catch {
                self.log.error("Change enumeration failed: \(logDescription(error), privacy: .public)")
                observer.finishEnumeratingWithError(WebDavClient.fileProviderError(for: error))
            }
        }
    }

    func currentSyncAnchor(completionHandler: @escaping (NSFileProviderSyncAnchor?) -> Void) {
        Task {
            completionHandler(Self.anchor(await self.context.store.currentAnchor()))
        }
    }
}
