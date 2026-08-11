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
import Foundation
import os.log

/// Connection settings for one File Provider domain.
///
/// Milestone 2/3 read them from `FileProviderConfig.json` in the shared app
/// group container - written by the client for testing. A later milestone
/// moves the secret into the keychain and pushes settings over XPC instead.
struct DavConfig: Codable {
    /// Full WebDAV root for this domain, e.g.
    /// `http://localhost:8080/remote.php/dav/files/admin` (oC10, single root) or
    /// a per-space `webDavUrl()` (oCIS).
    let webDavUrl: String
    let username: String
    /// Basic-auth password / app password (oC10). Optional so an OAuth config
    /// carries only a bearer token.
    let password: String?
    /// OAuth2/OIDC access token (oCIS). Optional; the client rewrites it when it
    /// refreshes.
    let bearerToken: String?

    enum CodingKeys: String, CodingKey {
        case webDavUrl, username, password, bearerToken
    }

    var rootURL: URL? { URL(string: webDavUrl) }

    /// Base for the fileid->path resolver (…/remote.php/dav/meta), derived from
    /// the files dav root by swapping the trailing "files/<user>" segment.
    var metaBaseURL: URL? {
        guard let url = rootURL, var comps = URLComponents(url: url, resolvingAgainstBaseURL: false) else {
            return nil
        }
        // …/remote.php/dav/files/<user>  ->  …/remote.php/dav/meta
        var parts = comps.path.split(separator: "/").map(String.init)
        if let filesIdx = parts.lastIndex(of: "files"), filesIdx + 1 < parts.count {
            parts.removeSubrange(filesIdx...)
            parts.append("meta")
        } else {
            return nil
        }
        comps.path = "/" + parts.joined(separator: "/")
        return comps.url
    }

    /// The Authorization header value: Bearer for OAuth (oCIS), Basic otherwise.
    var authHeader: String {
        if let token = bearerToken, !token.isEmpty {
            return "Bearer \(token)"
        }
        let raw = Data("\(username):\(password ?? "")".utf8).base64EncodedString()
        return "Basic \(raw)"
    }

    /// Basic auth over plain http leaks the password on the wire, so it is only
    /// allowed for a loopback test server; everything else must be https.
    var isTransportAcceptable: Bool {
        guard let url = rootURL else { return false }
        if url.scheme == "https" { return true }
        let host = url.host ?? ""
        return host == "localhost" || host == "127.0.0.1" || host == "::1"
    }

    /// The path part of the dav root, percent-decoded, used to turn hrefs into
    /// relative paths ("/remote.php/dav/files/admin").
    var rootPath: String {
        guard let url = rootURL else { return "" }
        return url.path.hasSuffix("/") ? String(url.path.dropLast()) : url.path
    }

    static func groupContainer() -> URL? {
        guard let ext = Bundle.main.object(forInfoDictionaryKey: "NSExtension") as? [String: Any],
              let group = ext["NSExtensionFileProviderDocumentGroup"] as? String
        else {
            return nil
        }
        return FileManager.default.containerURL(forSecurityApplicationGroupIdentifier: group)
    }

    /// Load the settings the client wrote for this domain. Prefers the
    /// per-domain file (multi-account / multi-space), falling back to a single
    /// shared file used by the local test harness.
    static func load(domainIdentifier: String? = nil) -> DavConfig? {
        guard let container = groupContainer() else { return nil }
        var candidates: [URL] = []
        if let id = domainIdentifier {
            candidates.append(container.appendingPathComponent("FileProviderConfig-\(id).json"))
        }
        candidates.append(container.appendingPathComponent("FileProviderConfig.json"))
        for file in candidates {
            if let data = try? Data(contentsOf: file),
               let config = try? JSONDecoder().decode(DavConfig.self, from: data) {
                return config
            }
        }
        return nil
    }
}

/// One entry of a PROPFIND response.
struct DavEntry {
    /// Path relative to the dav root, "" for the root itself.
    let path: String
    let name: String
    let isFolder: Bool
    let size: Int64
    let etag: String
    let modified: Date?
    /// Server-stable id (oc:fileid). Survives rename/move, which is exactly
    /// what NSFileProviderItemIdentifier needs.
    let fileId: String

    var identifier: NSFileProviderItemIdentifier {
        if path.isEmpty {
            return .rootContainer
        }
        return NSFileProviderItemIdentifier(fileId.isEmpty ? "p:\(path)" : fileId)
    }

    var parentPath: String {
        guard let slash = path.lastIndex(of: "/") else { return "" }
        return String(path[path.startIndex..<slash])
    }
}

enum DavError: Error {
    case badConfig
    case insecureTransport
    case http(Int)
    case parse
    case unknownItem
    /// Server rejected a conditional write (If-Match/If-None-Match) - the item
    /// changed underneath us.
    case precondition
}

extension Array where Element == DavEntry {
    /// Turn a listing into an id->etag snapshot without trapping on duplicate
    /// ids (a defensive measure against odd server responses).
    func etagSnapshot() -> [String: String] {
        var out: [String: String] = [:]
        for entry in self { out[entry.identifier.rawValue] = entry.etag }
        return out
    }
}

/// Persistent bookkeeping for one domain: id<->path plus per-container etag
/// snapshots (for change enumeration) and a monotonically increasing sync
/// anchor. Small JSON file in the app group container - sufficient for the
/// current tree sizes; a proper database arrives with the full sync engine.
actor ItemStore {
    private struct State: Codable {
        var pathById: [String: String] = [:]
        var childrenById: [String: [String: String]] = [:]
        var anchor: Int = 1
    }

    private var state = State()
    private let file: URL?
    /// True when a store file existed but could not be read back - we then lost
    /// our delta baseline and must ask the system to re-enumerate from scratch.
    let didReset: Bool

    init(domainIdentifier: String) {
        let url = DavConfig.groupContainer()?
            .appendingPathComponent("FileProviderStore-\(domainIdentifier).json")
        file = url
        if let url = url, let data = try? Data(contentsOf: url) {
            if let loaded = try? JSONDecoder().decode(State.self, from: data) {
                state = loaded
                didReset = false
            } else {
                didReset = true // corrupt - baseline is gone
            }
        } else {
            didReset = false // first run - a clean initial enumeration is expected
        }
    }

    private func save() {
        guard let file = file, let data = try? JSONEncoder().encode(state) else { return }
        try? data.write(to: file, options: .atomic)
    }

    func path(for identifier: NSFileProviderItemIdentifier) -> String? {
        if identifier == .rootContainer { return "" }
        return state.pathById[identifier.rawValue]
    }

    func identifier(forPath path: String) -> NSFileProviderItemIdentifier? {
        if path.isEmpty { return .rootContainer }
        for (id, p) in state.pathById where p == path {
            return NSFileProviderItemIdentifier(id)
        }
        return nil
    }

    func register(entries: [DavEntry]) {
        for entry in entries where !entry.path.isEmpty {
            state.pathById[entry.identifier.rawValue] = entry.path
        }
        save()
    }

    func bind(id: NSFileProviderItemIdentifier, path: String) {
        state.pathById[id.rawValue] = path
        save()
    }

    /// Record a rename/move including all descendants' paths.
    func move(id: NSFileProviderItemIdentifier, to newPath: String) {
        guard let oldPath = state.pathById[id.rawValue] else { return }
        state.pathById[id.rawValue] = newPath
        let prefix = oldPath + "/"
        for (other, p) in state.pathById where other != id.rawValue && p.hasPrefix(prefix) {
            state.pathById[other] = newPath + "/" + String(p.dropFirst(prefix.count))
        }
        save()
    }

    func remove(id: NSFileProviderItemIdentifier) {
        guard let path = state.pathById.removeValue(forKey: id.rawValue) else {
            save()
            return
        }
        let prefix = path + "/"
        for (other, p) in state.pathById where p.hasPrefix(prefix) {
            state.pathById.removeValue(forKey: other)
        }
        state.childrenById.removeValue(forKey: id.rawValue)
        save()
    }

    func children(of container: NSFileProviderItemIdentifier) -> [String: String] {
        state.childrenById[container.rawValue] ?? [:]
    }

    /// Every container we have enumerated so far - the working set's change
    /// enumeration walks these to find remote changes anywhere in the tree.
    func knownContainers() -> [NSFileProviderItemIdentifier] {
        state.childrenById.keys.map { NSFileProviderItemIdentifier($0) }
    }

    func setChildren(of container: NSFileProviderItemIdentifier, to snapshot: [String: String]) {
        state.childrenById[container.rawValue] = snapshot
        save()
    }

    func currentAnchor() -> Int {
        state.anchor
    }

    func bumpAnchor() -> Int {
        state.anchor += 1
        save()
        return state.anchor
    }
}

/// Minimal WebDAV client: PROPFIND/GET for reading, PUT/MKCOL/MOVE/DELETE for
/// writing.
final class WebDavClient {
    let config: DavConfig
    private let log = Logger(subsystem: "online.owncloud.desktopclient", category: "webdav")

    init(config: DavConfig) {
        self.config = config
    }

    private func url(for path: String) throws -> URL {
        guard var url = config.rootURL else { throw DavError.badConfig }
        if !path.isEmpty {
            for component in path.split(separator: "/") {
                url.appendPathComponent(String(component))
            }
        }
        return url
    }

    private func request(path: String, method: String, depth: String? = nil) throws -> URLRequest {
        var request = URLRequest(url: try url(for: path))
        request.httpMethod = method
        request.setValue(config.authHeader, forHTTPHeaderField: "Authorization")
        if let depth = depth {
            request.setValue(depth, forHTTPHeaderField: "Depth")
        }
        return request
    }

    /// A quoted etag for use in If-Match / If-None-Match.
    private func quoted(_ etag: String) -> String {
        let trimmed = etag.replacingOccurrences(of: "\"", with: "")
        return "\"\(trimmed)\""
    }

    private func expect(_ response: URLResponse, _ codes: Set<Int>, _ what: String) throws {
        guard let http = response as? HTTPURLResponse else { throw DavError.parse }
        guard codes.contains(http.statusCode) else {
            log.error("\(what, privacy: .public) -> HTTP \(http.statusCode)")
            if http.statusCode == 412 || http.statusCode == 409 {
                throw DavError.precondition
            }
            throw DavError.http(http.statusCode)
        }
    }

    /// Resolve a server file id to its current path via the dav/meta endpoint.
    /// Returns nil if the id no longer exists (404) - used to tell a real
    /// deletion apart from a server-side move.
    func resolvePath(fileId: String) async throws -> String? {
        guard !fileId.isEmpty, !fileId.hasPrefix("p:"), let base = config.metaBaseURL else {
            return nil
        }
        var url = base
        url.appendPathComponent(fileId)
        var request = URLRequest(url: url)
        request.httpMethod = "PROPFIND"
        request.setValue(config.authHeader, forHTTPHeaderField: "Authorization")
        request.setValue("0", forHTTPHeaderField: "Depth")
        request.setValue("application/xml", forHTTPHeaderField: "Content-Type")
        request.httpBody = Data("""
        <?xml version="1.0"?>
        <d:propfind xmlns:d="DAV:" xmlns:oc="http://owncloud.org/ns">
          <d:prop><oc:meta-path-for-user/></d:prop>
        </d:propfind>
        """.utf8)
        let (data, response) = try await URLSession.shared.data(for: request)
        guard let http = response as? HTTPURLResponse else { throw DavError.parse }
        if http.statusCode == 404 { return nil }
        guard http.statusCode == 207 else { throw DavError.http(http.statusCode) }
        // Extract <oc:meta-path-for-user>…</…>
        guard let xml = String(data: data, encoding: .utf8),
              let range = xml.range(of: "meta-path-for-user>"),
              let end = xml.range(of: "</", range: range.upperBound..<xml.endIndex)
        else {
            return nil
        }
        let path = String(xml[range.upperBound..<end.lowerBound])
        return path.trimmingCharacters(in: CharacterSet(charactersIn: "/"))
    }

    /// PROPFIND on `path`; returns the entry itself first, children after.
    func propfind(path: String, depth: Int) async throws -> [DavEntry] {
        var request = try request(path: path, method: "PROPFIND", depth: String(depth))
        request.setValue("application/xml", forHTTPHeaderField: "Content-Type")
        request.httpBody = Data("""
        <?xml version="1.0"?>
        <d:propfind xmlns:d="DAV:" xmlns:oc="http://owncloud.org/ns">
          <d:prop>
            <d:resourcetype/>
            <d:getcontentlength/>
            <d:getetag/>
            <d:getlastmodified/>
            <oc:fileid/>
          </d:prop>
        </d:propfind>
        """.utf8)

        let (data, response) = try await URLSession.shared.data(for: request)
        try expect(response, [207], "PROPFIND \(path)")
        return try MultistatusParser.parse(data: data, rootPath: config.rootPath)
    }

    /// Download a file to a temporary location and return that URL. The
    /// underlying URLSession task is attached to `progress` so the Finder can
    /// cancel a running hydration.
    func fetch(path: String, progress: Progress? = nil) async throws -> URL {
        let request = try request(path: path, method: "GET")
        let (tmp, response): (URL, URLResponse) = try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { continuation in
                let task = URLSession.shared.downloadTask(with: request) { url, response, error in
                    if let error = error {
                        continuation.resume(throwing: error)
                    } else if let url = url, let response = response {
                        // Move synchronously inside the handler: url is deleted
                        // as soon as this closure returns.
                        let dst = FileManager.default.temporaryDirectory
                            .appendingPathComponent(UUID().uuidString)
                        do {
                            try FileManager.default.moveItem(at: url, to: dst)
                            continuation.resume(returning: (dst, response))
                        } catch {
                            continuation.resume(throwing: error)
                        }
                    } else {
                        continuation.resume(throwing: DavError.parse)
                    }
                }
                progress?.addChild(task.progress, withPendingUnitCount: progress?.totalUnitCount ?? 1)
                task.resume()
            }
        } onCancel: {
            // The task is cancelled via the progress child above.
        }
        try expect(response, [200], "GET \(path)")
        return tmp
    }

    /// Upload file contents (or create an empty file). `ifMatch` guards against
    /// lost updates (send the base etag), `ifNoneMatch` = "*" refuses to
    /// overwrite an existing file (used for fresh creates).
    func put(path: String, contents: URL?, ifMatch: String? = nil, ifNoneMatch: String? = nil) async throws {
        var request = try request(path: path, method: "PUT")
        if let etag = ifMatch { request.setValue(quoted(etag), forHTTPHeaderField: "If-Match") }
        if let inm = ifNoneMatch { request.setValue(inm, forHTTPHeaderField: "If-None-Match") }
        let response: URLResponse
        if let contents = contents {
            (_, response) = try await URLSession.shared.upload(for: request, fromFile: contents)
        } else {
            (_, response) = try await URLSession.shared.upload(for: request, from: Data())
        }
        try expect(response, [200, 201, 204], "PUT \(path)")
    }

    /// Create a collection. Returns false if it already exists (405), true if
    /// freshly created - lets createItem bind to an existing folder instead of
    /// wedging on the deterministic error.
    @discardableResult
    func mkcol(path: String) async throws -> Bool {
        let request = try request(path: path, method: "MKCOL")
        let (_, response) = try await URLSession.shared.data(for: request)
        if let http = response as? HTTPURLResponse, http.statusCode == 405 {
            return false
        }
        try expect(response, [201], "MKCOL \(path)")
        return true
    }

    /// Move/rename. `overwrite` defaults to false so an existing destination is
    /// never silently clobbered (server answers 412).
    func move(path: String, to newPath: String, overwrite: Bool = false) async throws {
        var request = try request(path: path, method: "MOVE")
        request.setValue(try url(for: newPath).absoluteString, forHTTPHeaderField: "Destination")
        request.setValue(overwrite ? "T" : "F", forHTTPHeaderField: "Overwrite")
        let (_, response) = try await URLSession.shared.data(for: request)
        try expect(response, [201, 204], "MOVE \(path) -> \(newPath)")
    }

    /// Delete. `ifMatch` guards a file delete against a concurrent remote edit.
    func delete(path: String, ifMatch: String? = nil) async throws {
        var request = try request(path: path, method: "DELETE")
        if let etag = ifMatch { request.setValue(quoted(etag), forHTTPHeaderField: "If-Match") }
        let (_, response) = try await URLSession.shared.data(for: request)
        try expect(response, [204], "DELETE \(path)")
    }

    /// Map a transport/HTTP error onto the File Provider error the system expects.
    static func fileProviderError(for error: Error) -> Error {
        switch error {
        case DavError.unknownItem:
            return NSFileProviderError(.noSuchItem)
        case DavError.insecureTransport:
            return NSFileProviderError(.notAuthenticated)
        case DavError.precondition:
            // The item changed on the server; ask the system to re-sync it
            // rather than retrying the same stale write.
            return NSFileProviderError(.serverUnreachable)
        case DavError.http(let code):
            if code == 401 || code == 403 { return NSFileProviderError(.notAuthenticated) }
            if code == 404 { return NSFileProviderError(.noSuchItem) }
            if code == 507 { return NSFileProviderError(.insufficientQuota) }
            return NSFileProviderError(.serverUnreachable)
        default:
            return NSFileProviderError(.serverUnreachable)
        }
    }
}

/// Parses a DAV:multistatus document into entries.
private final class MultistatusParser: NSObject, XMLParserDelegate {
    private let rootPath: String
    private var entries: [DavEntry] = []

    private var href = ""
    private var isFolder = false
    private var size: Int64 = 0
    private var etag = ""
    private var fileId = ""
    private var modified: Date?
    private var text = ""

    private static let httpDate: DateFormatter = {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "EEE, dd MMM yyyy HH:mm:ss zzz"
        return f
    }()

    private init(rootPath: String) {
        self.rootPath = rootPath
    }

    static func parse(data: Data, rootPath: String) throws -> [DavEntry] {
        let delegate = MultistatusParser(rootPath: rootPath)
        let parser = XMLParser(data: data)
        parser.delegate = delegate
        parser.shouldProcessNamespaces = true
        guard parser.parse() else { throw DavError.parse }
        return delegate.entries
    }

    func parser(_ parser: XMLParser, didStartElement elementName: String, namespaceURI: String?,
                qualifiedName qName: String?, attributes attributeDict: [String: String] = [:]) {
        text = ""
        if elementName == "collection" {
            isFolder = true
        }
        if elementName == "response" {
            href = ""
            isFolder = false
            size = 0
            etag = ""
            fileId = ""
            modified = nil
        }
    }

    func parser(_ parser: XMLParser, foundCharacters string: String) {
        text += string
    }

    func parser(_ parser: XMLParser, didEndElement elementName: String, namespaceURI: String?,
                qualifiedName qName: String?) {
        switch elementName {
        case "href":
            href = text
        case "getcontentlength":
            size = Int64(text.trimmingCharacters(in: .whitespacesAndNewlines)) ?? 0
        case "getetag":
            etag = text.replacingOccurrences(of: "\"", with: "")
        case "fileid":
            fileId = text.trimmingCharacters(in: .whitespacesAndNewlines)
        case "getlastmodified":
            modified = Self.httpDate.date(from: text.trimmingCharacters(in: .whitespacesAndNewlines))
        case "response":
            if let entry = makeEntry() {
                entries.append(entry)
            }
        default:
            break
        }
    }

    private func makeEntry() -> DavEntry? {
        guard let decoded = href.removingPercentEncoding else { return nil }
        var path = decoded
        if path.hasPrefix(rootPath) {
            path = String(path.dropFirst(rootPath.count))
        }
        path = path.trimmingCharacters(in: CharacterSet(charactersIn: "/"))
        let name = path.split(separator: "/").last.map(String.init) ?? ""
        return DavEntry(path: path, name: name, isFolder: isFolder, size: size,
                        etag: etag, modified: modified, fileId: fileId)
    }
}
