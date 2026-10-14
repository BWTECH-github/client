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

#include "common/plugin.h"
#include "common/vfs.h"

namespace OCC {

/** macOS File Provider based VFS backend (experimental).
 *
 * The heavy lifting on macOS is done by a separate File Provider extension
 * (NSFileProviderReplicatedExtension) that the Finder talks to. This plugin is
 * the thin app-side bridge: it will register/unregister an NSFileProviderDomain
 * per synced folder and translate pin-state/availability queries, while the
 * extension serves enumeration and on-demand content.
 *
 * Current state: scaffolding. The Vfs interface is fully implemented with
 * db-backed pin states, but no File Provider domain is registered yet, so
 * enabling this mode behaves like plain sync. Domain registration and the
 * extension target land next; until then the mode is only reachable through
 * the explicit opt-in in bestAvailableVfsMode().
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
    /// Stable identifier of the NSFileProviderDomain backing this folder.
    QString _domainIdentifier;
};


class MacVfsPluginFactory : public QObject, public DefaultPluginFactory<VfsMac>
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.owncloud.PluginFactory" FILE "vfspluginmetadata.json")
    Q_INTERFACES(OCC::PluginFactory)
};

} // namespace OCC
