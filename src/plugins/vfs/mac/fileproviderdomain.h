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

#pragma once

#include <QByteArray>
#include <QString>
#include <QUrl>

#include <functional>

namespace OCC {
namespace Mac {

    /** State of a File Provider domain as macOS reports it. */
    enum class DomainResult {
        Ok,
        /** The extension exists but the user has not switched it on in
         *  System Settings > General > Login Items & Extensions > (app) > File Providers.
         *  Without that the domain stays dormant (FP -2011), so we surface it to the user. */
        ExtensionDisabled,
        /** No File Provider extension is registered for this app at all, e.g. the
         *  app runs from a disk image or a quarantined location (FP -2001). */
        ExtensionMissing,
        /** macOS is too old for our File Provider extension, or the app has none. */
        Unsupported,
        Failed,
    };

    /** Whether this macOS version can host our File Provider extension and the
     *  extension is embedded in the app bundle (Contents/PlugIns/FileProviderExt.appex). */
    bool fileProviderAvailable();

    /** Connection settings the extension reads for its domain
     *  (FileProviderExt/WebDavBackend.swift, DavConfig). No secrets: the
     *  Authorization header travels through the keychain (storeAuthorization()). */
    struct DomainSettings
    {
        /// WebDAV address of the domain root (the folder's remote path included)
        QUrl rootUrl;
        /// …/remote.php/dav/meta of the account, to tell a server-side move from a deletion
        QUrl metaUrl;
        /// the domain root below the user's files, "" for the whole account ("Projekte/2026")
        QString remoteRoot;
        QString user;
        QString userAgent;
    };

    /** Write the settings into the app group container the extension shares with us;
     *  the extension picks up a change (server move) with its next request. */
    bool writeDomainSettings(const QString &identifier, const DomainSettings &settings);
    /** Remove the settings and the extension's bookkeeping of the domain. */
    void removeDomainSettings(const QString &identifier);

    /** Keep the value of the Authorization header ("Bearer …" or "Basic …") for the
     *  extension in the login keychain, readable only by this app and its File
     *  Provider extension. The client keeps refreshing tokens; the extension only
     *  reads what the client last used successfully. */
    bool storeAuthorization(const QString &identifier, const QByteArray &authorization);
    void removeAuthorization(const QString &identifier);

    /** Register the domain for a synced folder, or keep the existing one.
     *
     * Asynchronous: the callback runs on the main thread once macOS has
     * answered. An existing domain is never removed here: that would throw away
     * downloaded files and local changes that are not uploaded yet.
     */
    void registerDomain(const QString &identifier, const QString &displayName,
        std::function<void(DomainResult result, const QString &errorMessage)> callback);

    /** Ask macOS whether the domain can be used and where the Finder shows it
     *  (~/Library/CloudStorage/…). The callback runs on the main thread. */
    void queryDomain(const QString &identifier, std::function<void(DomainResult result, const QString &userVisiblePath)> callback);

    /** Remove the domain, e.g. when the folder sync or the account is removed.
     *  Files with changes that never reached the server are preserved by macOS. */
    void unregisterDomain(const QString &identifier, std::function<void()> callback = {});

    /** Remove domains of this app that no folder of the client owns any more: every folder writes
     *  its settings (writeDomainSettings()) before it registers its domain and removes them with
     *  it, so a domain without settings is left over (an older client version, a lost
     *  configuration). Unsynced changes in it are preserved by macOS. */
    void removeStaleDomains();

    /** Disconnect the domain with @p reason, which the Finder shows (signed out,
     *  paused): macOS keeps the files but stops talking to the extension. An empty
     *  reason connects it again. Does nothing if the domain already is in that state. */
    void setDomainSuspended(const QString &identifier, const QString &reason, bool temporary);

    /** Tell the extension that something changed on the server. */
    void signalChanges(const QString &identifier);

    /** Tell the extension that new credentials (or settings) are available, or the
     *  server is reachable again: operations that failed for that are retried now. */
    void signalErrorsResolved(const QString &identifier);

    /** Call @p handler (on the main thread) whenever an extension asks for fresh
     *  credentials because the server rejected the ones it has. Returns a token for
     *  stopObservingCredentialRequests(). */
    int observeCredentialRequests(std::function<void()> handler);
    void stopObservingCredentialRequests(int token);

    /** Human readable hint for a non-Ok result, ready to show in the GUI. */
    QString domainResultMessage(DomainResult result);

} // namespace Mac
} // namespace OCC
