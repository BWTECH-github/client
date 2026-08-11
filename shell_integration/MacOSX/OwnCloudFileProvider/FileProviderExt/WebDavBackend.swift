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
/// Milestone 2 reads them from `FileProviderConfig.json` in the shared app
/// group container - written by the client for testing. Milestone 3 moves the
/// secret into the keychain and pushes settings over XPC instead.
struct DavConfig: Codable {
    /// Full WebDAV root for this domain, e.g.
    /// `http://localhost:8080/remote.php/dav/files/admin`.
    let webDavUrl: String
    let username: String
    let password: String

    var rootURL: URL? { URL(string: webDavUrl) }

    var basicAuthHeader: String {
        let raw = Data("\(username):\(password)".utf8).base64EncodedString()
        return "Basic \(raw)"
    }

    /// The path part of the dav root, percent-decoded, used to turn hrefs into
    /// item identifiers ("/remote.php/dav/files/admin").
    var rootPath: String {
        guard let url = rootURL else { return "" }
        return url.path.hasSuffix("/") ? String(url.path.dropLast()) : url.path
    }

    static func load() -> DavConfig? {
        guard let ext = Bundle.main.object(forInfoDictionaryKey: "NSExtension") as? [String: Any],
              let group = ext["NSExtensionFileProviderDocumentGroup"] as? String,
              let container = FileManager.default.containerURL(forSecurityApplicationGroupIdentifier: group)
        else {
            return nil
        }
        let file = container.appendingPathComponent("FileProviderConfig.json")
        guard let data = try? Data(contentsOf: file) else { return nil }
        return try? JSONDecoder().decode(DavConfig.self, from: data)
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

    var identifier: NSFileProviderItemIdentifier {
        path.isEmpty ? .rootContainer : NSFileProviderItemIdentifier(path)
    }

    var parentIdentifier: NSFileProviderItemIdentifier {
        guard let slash = path.lastIndex(of: "/") else {
            return .rootContainer
        }
        return NSFileProviderItemIdentifier(String(path[path.startIndex..<slash]))
    }
}

enum DavError: Error {
    case badConfig
    case http(Int)
    case parse
}

/// Minimal WebDAV client: PROPFIND for metadata, GET for content.
final class WebDavClient {
    let config: DavConfig
    private let log = Logger(subsystem: "online.owncloud.desktopclient", category: "webdav")

    init(config: DavConfig) {
        self.config = config
    }

    private func request(path: String, method: String, depth: String? = nil) throws -> URLRequest {
        guard var url = config.rootURL else { throw DavError.badConfig }
        if !path.isEmpty {
            for component in path.split(separator: "/") {
                url.appendPathComponent(String(component))
            }
        }
        var request = URLRequest(url: url)
        request.httpMethod = method
        request.setValue(config.basicAuthHeader, forHTTPHeaderField: "Authorization")
        if let depth = depth {
            request.setValue(depth, forHTTPHeaderField: "Depth")
        }
        return request
    }

    /// PROPFIND on `path`; returns the entry itself first, children after.
    func propfind(path: String, depth: Int) async throws -> [DavEntry] {
        var request = try request(path: path, method: "PROPFIND", depth: String(depth))
        request.setValue("application/xml", forHTTPHeaderField: "Content-Type")
        request.httpBody = Data("""
        <?xml version="1.0"?>
        <d:propfind xmlns:d="DAV:">
          <d:prop>
            <d:resourcetype/>
            <d:getcontentlength/>
            <d:getetag/>
            <d:getlastmodified/>
          </d:prop>
        </d:propfind>
        """.utf8)

        let (data, response) = try await URLSession.shared.data(for: request)
        guard let http = response as? HTTPURLResponse else { throw DavError.parse }
        guard http.statusCode == 207 else {
            log.error("PROPFIND \(path, privacy: .public) -> HTTP \(http.statusCode)")
            throw DavError.http(http.statusCode)
        }
        return try MultistatusParser.parse(data: data, rootPath: config.rootPath)
    }

    /// Download a file to a temporary location and return that URL.
    func fetch(path: String) async throws -> URL {
        let request = try request(path: path, method: "GET")
        let (tmp, response) = try await URLSession.shared.download(for: request)
        guard let http = response as? HTTPURLResponse, http.statusCode == 200 else {
            let code = (response as? HTTPURLResponse)?.statusCode ?? -1
            log.error("GET \(path, privacy: .public) -> HTTP \(code)")
            throw DavError.http(code)
        }
        // Move out of URLSession's short-lived location.
        let dst = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString)
        try FileManager.default.moveItem(at: tmp, to: dst)
        return dst
    }

    /// Map a transport/HTTP error onto the File Provider error the system expects.
    static func fileProviderError(for error: Error) -> Error {
        if case DavError.http(let code) = error {
            if code == 401 || code == 403 {
                return NSFileProviderError(.notAuthenticated)
            }
            if code == 404 {
                return NSFileProviderError(.noSuchItem)
            }
        }
        return NSFileProviderError(.serverUnreachable)
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
        return DavEntry(path: path, name: name, isFolder: isFolder, size: size, etag: etag, modified: modified)
    }
}
