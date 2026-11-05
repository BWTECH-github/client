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

#include <QObject>
#include <QTimer>

#include "common/plugin.h"
#include "common/vfs.h"
#include "fileproviderdomain.h"

namespace OCC {

/** macOS File Provider based VFS backend.
 *
 * The Finder talks to the File Provider extension of the app
 * (NSFileProviderReplicatedExtension, shell_integration/MacOSX/OwnCloudFileProvider):
 * it lists, downloads and uploads on its own, the sync engine of the folder stays idle.
 * This plugin is the app side of it:
 *  - one NSFileProviderDomain per folder, kept across restarts and server moves, removed
 *    with the folder (or the account),
 *  - the connection settings for the extension in the shared app group container, the
 *    Authorization header of the account in the keychain (the client refreshes tokens, the
 *    extension never does, so they cannot race for a refresh token); an extension that got
 *    a 401 asks for fresh ones,
 *  - every remotePollInterval the ETag of the folder on the server; a change, new
 *    credentials or "Sync now" make the extension look for changes,
 *  - signed out or paused: the domain is disconnected (the Finder says why), and signed out
 *    the extension loses its credentials,
 *  - the state of the domain (switched off in System Settings, …) for the folder status.
 */
class VfsMac : public Vfs
{
    Q_OBJECT

public:
    VfsMac(QObject *parent = nullptr);
    ~VfsMac() override;

    Mode mode() const override;

    void stop() override;
    void unregisterFolder() override;

    bool socketApiPinStateActionsShown() const override;

    void fileProviderRefresh() override;
    void fileProviderSetSuspended(const QString &reason, bool temporary) override;
    QString fileProviderProblem(bool *needsSystemSettings = nullptr) const override;
    QString fileProviderPath() const override;

    Result<void, QString> createPlaceholder(const SyncFileItem &) override;

    bool needsMetadataUpdate(const SyncFileItem &) override;
    bool isDehydratedPlaceholder(const QString &) override;
    bool statTypeVirtualFile(csync_file_stat_t *, void *) override;

    bool setPinState(const QString &folderPath, PinState state) override;
    Optional<PinState> pinState(const QString &folderPath) override;
    AvailabilityResult availability(const QString &folderPath) override;

public Q_SLOTS:
    void onFileStatusChanged(const QString &, SyncFileStatus) override;

protected:
    Result<ConvertToPlaceholderResult, QString> updateMetadata(const SyncFileItem &, const QString &, const QString &) override;
    void startImpl(const VfsSetupParams &) override;

private:
    void poll();
    void resendKeptItems();
    void checkDomain();
    void applySuspension();
    void mirrorAuthorization(const QByteArray &authorization);
    void dropAuthorization();

    /// Stable identifier of the NSFileProviderDomain backing this folder.
    QString _domainIdentifier;
    /// macOS answered the registration
    bool _registered = false;
    Mac::DomainResult _state = Mac::DomainResult::Ok;
    QString _userVisiblePath;
    QTimer _pollTimer;
    QString _lastEtag;
    bool _pollRunning = false;
    bool _lastPollFailed = false;
    /// what the keychain holds for the extension, empty if nothing
    QByteArray _mirroredAuthorization;
    int _pollsWithoutCredentials = 0;
    /// why the domain is disconnected (signed out, paused), empty while it is connected
    QString _suspendReason;
    bool _suspendTemporary = true;
    bool _suspendApplied = false;
    int _credentialRequestToken = 0;
};

class MacVfsPluginFactory : public QObject, public DefaultPluginFactory<VfsMac>
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.owncloud.PluginFactory" FILE "vfspluginmetadata.json")
    Q_INTERFACES(OCC::PluginFactory)
};

} // namespace OCC
