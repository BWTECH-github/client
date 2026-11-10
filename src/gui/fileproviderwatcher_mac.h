/*
 * Copyright (C) BW-Tech GmbH
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

#include "gui/owncloudguilib.h"

#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>

#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>

namespace OCC {

/**
 * Meldet Änderungen im sichtbaren Ordner einer File-Provider-Domain (~/Library/CloudStorage/…).
 *
 * Lädt der Finder eine Datei oder gibt ihren Speicher frei, ändert macOS sie dort, ohne dass der Client davon erfährt: Der
 * Ordnerwächter eines File-Provider-Ordners sieht nur dessen Journal. Gemeldet werden Pfade relativ zum Ordner, je Sekunde
 * gesammelt. FSEvents ruft auf einer eigenen Warteschlange zurück, nicht im Run Loop: Den bedient Qts Ereignisschleife nicht
 * in jeder Umgebung (Tests, Plattform „offscreen“).
 */
class OWNCLOUDGUI_EXPORT FileProviderWatcher : public QObject
{
    Q_OBJECT
public:
    FileProviderWatcher(const QString &root, QObject *parent);
    ~FileProviderWatcher() override;

    QString root() const { return _root; }

Q_SIGNALS:
    void changed(const QSet<QString> &relativePaths);
    /// FSEvents hat Ereignisse zusammengefasst oder verloren: alles unter relativeDir neu ansehen ("" für den ganzen Ordner)
    void rescanNeeded(const QString &relativeDir);

private:
    static void callback(ConstFSEventStreamRef stream, void *info, size_t count, void *paths, const FSEventStreamEventFlags flags[],
        const FSEventStreamEventId ids[]);
    void collect(const QSet<QString> &relativePaths, const QSet<QString> &rescans);

    const QString _root;
    // ohne Verknüpfungen, so meldet FSEvents die Pfade (/var liegt zum Beispiel unter /private/var)
    const QString _canonicalRoot;
    dispatch_queue_t _queue = nullptr;
    FSEventStreamRef _stream = nullptr;
    QSet<QString> _pending;
    QSet<QString> _pendingRescans;
    QTimer _timer;
};

}
