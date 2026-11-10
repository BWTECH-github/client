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

#include "fileproviderwatcher_mac.h"

#include <QFileInfo>
#include <QLoggingCategory>

#include <utility>

namespace OCC {

Q_LOGGING_CATEGORY(lcFileProviderWatcher, "gui.fileproviderwatcher", QtInfoMsg)

FileProviderWatcher::FileProviderWatcher(const QString &root, QObject *parent)
    : QObject(parent)
    , _root(root)
    , _canonicalRoot(QFileInfo(root).canonicalFilePath().normalized(QString::NormalizationForm_C))
{
    _timer.setSingleShot(true);
    _timer.setInterval(1000);
    connect(&_timer, &QTimer::timeout, this, [this] {
        qCDebug(lcFileProviderWatcher) << "Changed in" << _root << ":" << _pending << "rescan:" << _pendingRescans;
        const auto rescans = std::exchange(_pendingRescans, {});
        for (const QString &dir : rescans) {
            Q_EMIT rescanNeeded(dir);
        }
        if (!_pending.isEmpty()) {
            Q_EMIT changed(std::exchange(_pending, {}));
        }
    });

    if (_canonicalRoot.isEmpty()) {
        qCWarning(lcFileProviderWatcher) << "Cannot watch" << root << "because it does not exist";
        return;
    }

    const CFStringRef cfRoot = _canonicalRoot.toCFString();
    const void *values[] = {cfRoot};
    const CFArrayRef paths = CFArrayCreate(nullptr, values, 1, &kCFTypeArrayCallBacks);
    FSEventStreamContext context = {0, this, nullptr, nullptr, nullptr};
    _stream = FSEventStreamCreate(nullptr, &FileProviderWatcher::callback, &context, paths, kFSEventStreamEventIdSinceNow, 0.3,
        kFSEventStreamCreateFlagUseCFTypes | kFSEventStreamCreateFlagFileEvents);
    CFRelease(paths);
    CFRelease(cfRoot);
    if (!_stream) {
        qCWarning(lcFileProviderWatcher) << "Cannot watch" << _canonicalRoot;
        return;
    }
    _queue = dispatch_queue_create("online.owncloud.desktopclient.fileproviderwatcher", DISPATCH_QUEUE_SERIAL);
    FSEventStreamSetDispatchQueue(_stream, _queue);
    if (!FSEventStreamStart(_stream)) {
        qCWarning(lcFileProviderWatcher) << "Cannot start watching" << _canonicalRoot;
    }
}

FileProviderWatcher::~FileProviderWatcher()
{
    if (_stream) {
        FSEventStreamStop(_stream);
        FSEventStreamInvalidate(_stream);
    }
    if (_queue) {
        // ein Rückruf, der gerade läuft, endet vor dem Stream und dem Objekt; was er schon an die Ereignisschleife gab, verwirft Qt
        dispatch_sync_f(_queue, nullptr, [](void *) { });
    }
    if (_stream) {
        FSEventStreamRelease(_stream);
    }
    if (_queue) {
        dispatch_release(_queue);
    }
}

void FileProviderWatcher::callback(ConstFSEventStreamRef, void *info, size_t count, void *paths, const FSEventStreamEventFlags flags[],
    const FSEventStreamEventId[])
{
    // Laden meldet macOS als Modified (mit InodeMetaMod), Freigeben als Modified (mit XattrMod); Etiketten und Ähnliches zählen nicht
    const FSEventStreamEventFlags interesting = kFSEventStreamEventFlagItemCreated | kFSEventStreamEventFlagItemRemoved
        | kFSEventStreamEventFlagItemRenamed | kFSEventStreamEventFlagItemModified | kFSEventStreamEventFlagItemInodeMetaMod
        | kFSEventStreamEventFlagMustScanSubDirs;
    auto *self = static_cast<FileProviderWatcher *>(info);
    const QString prefix = self->_canonicalRoot + QLatin1Char('/');
    const auto eventPaths = static_cast<CFArrayRef>(paths);
    QSet<QString> relativePaths;
    QSet<QString> rescans;
    for (size_t i = 0; i < count; ++i) {
        if (!(flags[i] & interesting)) {
            continue;
        }
        const auto path = QString::fromCFString(static_cast<CFStringRef>(CFArrayGetValueAtIndex(eventPaths, static_cast<CFIndex>(i))))
                              .normalized(QString::NormalizationForm_C);
        const bool below = path.size() > prefix.size() && path.startsWith(prefix);
        if (flags[i] & kFSEventStreamEventFlagMustScanSubDirs) {
            // zusammengefasst oder verloren (bei vielen Änderungen auf einmal): der Ordner samt allem darin, sonst der ganze
            QString dir = below ? path.mid(prefix.size()) : QString();
            while (dir.endsWith(QLatin1Char('/'))) {
                dir.chop(1);
            }
            rescans.insert(dir);
        } else if (below) {
            relativePaths.insert(path.mid(prefix.size()));
        }
    }
    if (!relativePaths.isEmpty() || !rescans.isEmpty()) {
        QMetaObject::invokeMethod(self, [self, relativePaths, rescans] { self->collect(relativePaths, rescans); }, Qt::QueuedConnection);
    }
}

void FileProviderWatcher::collect(const QSet<QString> &relativePaths, const QSet<QString> &rescans)
{
    _pending.unite(relativePaths);
    _pendingRescans.unite(rescans);
    if (!_timer.isActive()) {
        _timer.start();
    }
}

}
