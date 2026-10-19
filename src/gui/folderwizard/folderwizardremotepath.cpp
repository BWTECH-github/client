/*
 * Copyright (C) by Hannah von Reth <hannah.vonreth@owncloud.com>
 * Copyright (C) by Duncan Mac-Vicar P. <duncan@kde.org>
 * Copyright (C) by BW-Tech GmbH for owncloud.online.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */
#include "folderwizardremotepath.h"
#include "ui_folderwizardtargetpage.h"

#include "common/utility.h"
#include "csync_exclude.h"
#include "gui/folderman.h"
#include "libsync/account.h"
#include "libsync/configfile.h"
#include "libsync/networkjobs.h"
#include "libsync/theme.h"
#include "resources/iconresources.h"

#include <QFileInfo>
#include <QHeaderView>
#include <QInputDialog>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QSignalBlocker>
#include <QTreeWidget>

#include <optional>

// csync_exclude.cpp, exportiert: Gerätenamen wie CON, NUL, COM1 (auch mit Endung) und Windows-Systemordner
extern bool csync_is_windows_reserved_word(QStringView filename);

using namespace OCC;

// eigene Kategorie: lcFolderWizard lebt in folderwizard.cpp, das die Seite für die Tests nicht mitziehen soll
Q_LOGGING_CATEGORY(lcFolderWizardRemotePath, "gui.folderwizard.remotepath", QtInfoMsg)

namespace {

// Zeichen, die Windows in Datei- und Ordnernamen nicht zulässt: ein solcher Ordner ließe sich dort nicht synchronisieren
const QString invalidFolderNameCharsC = QStringLiteral("\\/:*?\"<>|");

QTreeWidgetItem *findChildByName(QTreeWidgetItem *parent, const QString &name)
{
    for (int i = 0; i < parent->childCount(); ++i) {
        if (parent->child(i)->text(0) == name) {
            return parent->child(i);
        }
    }
    return nullptr;
}

QString childPath(const QString &parentPath, const QString &name)
{
    if (parentPath == QLatin1String("/")) {
        return QLatin1Char('/') + name;
    }
    return parentPath + QLatin1Char('/') + name;
}

/// zerlegt einen Serverpfad in seine Teile; ohne Ergebnis, wenn er „.“ oder „..“ enthält
std::optional<QStringList> splitRemotePath(const QString &path)
{
    const QStringList parts = path.trimmed().split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        if (part == QLatin1String(".") || part == QLatin1String("..")) {
            return {};
        }
    }
    return parts;
}

}

FolderWizardRemotePath::FolderWizardRemotePath(FolderWizardPrivate *parent, Account *account, const QUrl &davUrl)
    : FolderWizardPage(parent)
    , _ui(new Ui_FolderWizardTargetPage)
    , _account(account)
    , _davUrl(davUrl)
{
    _ui->setupUi(this);
    _ui->warningIcon->setPixmap(IconResources::getCoreIcon(QStringLiteral("warning")).pixmap(_ui->warningIcon->size()));
    _ui->warningLabel->setTextFormat(Qt::RichText);
    _ui->warningLabel->setWordWrap(true);
    _ui->warningFrame->hide();

    _ui->folderTreeWidget->setSortingEnabled(true);
    _ui->folderTreeWidget->sortByColumn(0, Qt::AscendingOrder);
    _ui->folderTreeWidget->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    // ohne gestreckte letzte Spalte bekommt der Baum eine Bildlaufleiste, wenn die Namen zu breit werden
    _ui->folderTreeWidget->header()->setStretchLastSection(false);
    _ui->folderTreeWidget->setAccessibleName(tr("Folders on the server"));
    _ui->folderEntry->setPlaceholderText(tr("Path on the server, for example /Documents"));
    _ui->folderEntry->setAccessibleName(tr("Path of the server folder"));

    connect(_ui->addFolderButton, &QAbstractButton::clicked, this, &FolderWizardRemotePath::askForNewFolderName);
    connect(_ui->refreshButton, &QAbstractButton::clicked, this, &FolderWizardRemotePath::refreshFolders);
    connect(_ui->folderTreeWidget, &QTreeWidget::itemExpanded, this, &FolderWizardRemotePath::onItemExpanded);
    connect(_ui->folderTreeWidget, &QTreeWidget::currentItemChanged, this, &FolderWizardRemotePath::onCurrentItemChanged);
    connect(_ui->folderEntry, &QLineEdit::textEdited, this, &FolderWizardRemotePath::onFolderEntryEdited);

    // eingetippte Pfade erst nach einer Tipp-Pause nachschlagen, nicht bei jedem Tastendruck
    _lscolTimer.setInterval(500);
    _lscolTimer.setSingleShot(true);
    connect(&_lscolTimer, &QTimer::timeout, this, &FolderWizardRemotePath::listTypedFolder);
}

FolderWizardRemotePath::~FolderWizardRemotePath()
{
    delete _ui;
}

void FolderWizardRemotePath::initializePage()
{
    _info.clear();
    _validityError.clear();
    refreshFolders();
}

void FolderWizardRemotePath::cleanupPage()
{
    _lscolTimer.stop();
    _info.clear();
    _validityError.clear();
    updateMessage();
}

QString FolderWizardRemotePath::targetPath() const
{
    const QTreeWidgetItem *item = _ui->folderTreeWidget->currentItem();
    return item ? pathForItem(item) : QString();
}

QString FolderWizardRemotePath::pathForItem(const QTreeWidgetItem *item)
{
    return item->data(0, Qt::UserRole).toString();
}

bool FolderWizardRemotePath::isComplete() const
{
    const QTreeWidgetItem *item = _ui->folderTreeWidget->currentItem();
    if (!item || !_account) {
        _validityError.clear();
        updateMessage();
        return false;
    }

    _validityError = FolderMan::instance()->checkRemotePathValidity(_account->uuid(), pathForItem(item));
    updateMessage();
    return _validityError.isEmpty();
}

QString FolderWizardRemotePath::checkNewFolderName(const QString &name)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
        return tr("Please enter a name for the new folder.");
    }
    if (trimmed == QLatin1String(".") || trimmed == QLatin1String("..")) {
        return tr("The folder name %1 is not allowed.").arg(trimmed);
    }
    for (const QChar c : invalidFolderNameCharsC) {
        if (trimmed.contains(c)) {
            QStringList chars;
            for (const QChar invalid : invalidFolderNameCharsC) {
                chars.append(invalid);
            }
            return tr("The folder name must not contain any of these characters: %1").arg(chars.join(QLatin1Char(' ')));
        }
    }
    // Windows schneidet einen Punkt am Ende ab, der Ordner hieße dort anders als auf dem Server
    if (trimmed.endsWith(QLatin1Char('.'))) {
        return tr("The folder name must not end with a dot.");
    }
    // unter Windows nicht anlegbar, auch wenn der Server ihn annimmt
    if (csync_is_windows_reserved_word(trimmed)) {
        return tr("The folder name %1 is reserved by Windows.").arg(trimmed);
    }
    // ein Ordner, den der Client selbst ausschließt (etwa „._…“ oder „….part“), würde nie synchronisiert
    ExcludedFiles excludes;
    ConfigFile::setupDefaultExcludeFilePaths(excludes);
    const QString remotePath = QLatin1Char('/') + trimmed;
    if (excludes.reloadExcludeFiles() && excludes.isExcludedRemote(remotePath, QStringLiteral("/"), false, ItemTypeDirectory)) {
        return tr("Folders named %1 are excluded from synchronization.").arg(trimmed);
    }
    return {};
}

QString FolderWizardRemotePath::suggestLocalPath(const QString &defaultSyncRoot, const QString &remotePath, const QUuid &accountUuid)
{
    const QFileInfo syncRoot(defaultSyncRoot);
    const QString remoteName = displayNameForRemotePath(remotePath);
    //: Name eines weiteren lokalen Ordners: %1 ist der Sync-Ordner des Kontos, %2 der Serverordner
    const QString name = remoteName.isEmpty() ? syncRoot.fileName() : tr("%1 - %2").arg(syncRoot.fileName(), remoteName);
    return FolderMan::instance()->findGoodPathForNewSyncFolder(syncRoot.path(), name, FolderMan::NewFolderType::SpacesFolder, accountUuid);
}

QString FolderWizardRemotePath::displayNameForRemotePath(const QString &remotePath)
{
    const auto trail = splitRemotePath(remotePath);
    return trail && !trail->isEmpty() ? trail->last() : QString();
}

void FolderWizardRemotePath::askForNewFolderName()
{
    const QString parentPath = targetPath().isEmpty() ? QStringLiteral("/") : targetPath();
    auto *dialog = new QInputDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Create folder on the server"));
    dialog->setLabelText(tr("Name of the new folder in %1:").arg(parentPath == QLatin1String("/") ? Theme::instance()->appNameGUI() : parentPath));
    connect(dialog, &QInputDialog::textValueSelected, this, &FolderWizardRemotePath::createRemoteFolder);
    dialog->open();
}

void FolderWizardRemotePath::createRemoteFolder(const QString &name)
{
    const QString nameError = checkNewFolderName(name);
    if (!nameError.isEmpty() || !_account) {
        setInfo(nameError);
        Q_EMIT remoteFolderCreated(false);
        return;
    }

    const QString parentPath = targetPath().isEmpty() ? QStringLiteral("/") : targetPath();
    const QString fullPath = childPath(parentPath, name.trimmed());

    auto *job = new MkColJob(_account, _davUrl, fullPath, {}, this);
    connect(job, &MkColJob::finishedWithoutError, this, [fullPath, this] {
        qCInfo(lcFolderWizardRemotePath) << "created remote folder" << fullPath;
        // direkt eintragen und auswählen, ohne den Baum neu zu laden
        insertPath(rootItem(), fullPath.split(QLatin1Char('/'), Qt::SkipEmptyParts));
        selectByPath(fullPath);
        setInfo(tr("The folder %1 was created on the server.").arg(fullPath));
        Q_EMIT remoteFolderCreated(true);
    });
    connect(job, &MkColJob::finishedWithError, this, [job, this](QNetworkReply *) {
        qCWarning(lcFolderWizardRemotePath) << "creating the remote folder failed:" << job->httpStatusCode() << job->errorString();
        switch (job->httpStatusCode()) {
        case 401:
            setInfo(tr("Authentication failed while accessing the server. Please sign in again and retry."));
            break;
        case 403:
            setInfo(tr("You are not allowed to create a folder here."));
            break;
        case 405:
            // WebDAV antwortet so auf MKCOL für einen schon vorhandenen Eintrag
            setInfo(tr("A folder or file with this name already exists on the server."));
            break;
        default:
            setInfo(tr("The folder could not be created on the server: %1").arg(job->errorStringParsingBody()));
            break;
        }
        Q_EMIT remoteFolderCreated(false);
    });
    job->start();
}

void FolderWizardRemotePath::refreshFolders()
{
    _lscolTimer.stop();
    _ui->folderTreeWidget->clear();
    _ui->folderEntry->clear();
    runPropFindJob(QStringLiteral("/"));
    Q_EMIT completeChanged();
}

PropfindJob *FolderWizardRemotePath::runPropFindJob(const QString &path)
{
    if (!_account) {
        return nullptr;
    }
    auto *job = new PropfindJob(_account, _davUrl, path, PropfindJob::Depth::One, this);
    job->setProperties({QByteArrayLiteral("resourcetype")});
    connect(job, &PropfindJob::directoryListingSubfolders, this, [path, this](const QStringList &hrefs) { updateDirectories(path, hrefs); });
    connect(job, &PropfindJob::finishedWithError, this, [job, this] { handleListingError(job); });
    job->start();
    return job;
}

void FolderWizardRemotePath::updateDirectories(const QString &listedPath, const QStringList &hrefs)
{
    // die Antwort nennt Pfade ab der Serverwurzel („/remote.php/dav/files/<Benutzer>/Dokumente/“), der Baum führt sie ab
    // der WebDAV-Wurzel des Kontos
    const QString davRoot = Utility::ensureTrailingSlash(_davUrl.path(QUrl::FullyDecoded));
    QTreeWidgetItem *root = rootItem();
    QStringList sorted = hrefs;
    Utility::sortFilenames(sorted);
    for (const QString &href : std::as_const(sorted)) {
        if (!href.startsWith(davRoot)) {
            continue;
        }
        if (const auto trail = splitRemotePath(href.mid(davRoot.size()))) {
            insertPath(root, *trail);
        }
    }
    {
        // aufklappen, ohne itemExpanded auszulösen: die Wurzel ist gerade gelistet worden
        const QSignalBlocker blocker(_ui->folderTreeWidget);
        root->setExpanded(true);
    }

    // ein Ordner ohne Unterordner verliert sein Aufklapp-Dreieck
    if (QTreeWidgetItem *listed = itemForPath(listedPath)) {
        listed->setChildIndicatorPolicy(QTreeWidgetItem::DontShowIndicatorWhenChildless);
    }
    Q_EMIT listingFinished();
}

void FolderWizardRemotePath::handleListingError(PropfindJob *job)
{
    // 404 beim Eintippen eines Pfads ist kein Fehler: den Ordner gibt es (noch) nicht, „Weiter“ bleibt gesperrt
    if (job->httpStatusCode() == 404) {
        setInfo(QString());
    } else {
        setInfo(tr("The folder list could not be loaded: %1").arg(job->errorStringParsingBody()));
    }
    Q_EMIT listingFinished();
}

void FolderWizardRemotePath::onItemExpanded(QTreeWidgetItem *item)
{
    runPropFindJob(pathForItem(item));
}

void FolderWizardRemotePath::onCurrentItemChanged(QTreeWidgetItem *item)
{
    if (item) {
        _ui->folderEntry->setText(pathForItem(item));
    }
    Q_EMIT completeChanged();
}

void FolderWizardRemotePath::onFolderEntryEdited(const QString &text)
{
    if (selectByPath(text)) {
        _lscolTimer.stop();
        return;
    }

    _ui->folderTreeWidget->setCurrentItem(nullptr);
    _lscolTimer.start();
}

void FolderWizardRemotePath::listTypedFolder()
{
    const auto trail = splitRemotePath(_ui->folderEntry->text());
    if (!trail) {
        return;
    }
    const QString path = QLatin1Char('/') + trail->join(QLatin1Char('/'));
    if (PropfindJob *job = runPropFindJob(path)) {
        // nach dem Eintragen (erste Verbindung oben) den Ordner auswählen, falls es ihn gibt
        connect(job, &PropfindJob::directoryListingSubfolders, this, [path, this] { selectByPath(path); });
    }
}

QTreeWidgetItem *FolderWizardRemotePath::rootItem()
{
    QTreeWidgetItem *root = _ui->folderTreeWidget->topLevelItem(0);
    if (!root) {
        root = new QTreeWidgetItem(_ui->folderTreeWidget);
        root->setText(0, Theme::instance()->appNameGUI());
        root->setIcon(0, IconResources::getBrandingIcon(Theme::instance()->applicationIconName()));
        root->setToolTip(0, tr("Choose this to sync the entire account"));
        root->setData(0, Qt::UserRole, QStringLiteral("/"));
    }
    return root;
}

void FolderWizardRemotePath::insertPath(QTreeWidgetItem *parent, const QStringList &pathTrail)
{
    QTreeWidgetItem *current = parent;
    for (const QString &name : pathTrail) {
        QTreeWidgetItem *child = findChildByName(current, name);
        if (!child) {
            const QString path = childPath(pathForItem(current), name);
            child = new QTreeWidgetItem(current);
            child->setIcon(0, IconResources::getCoreIcon(QStringLiteral("folder-sync")));
            child->setText(0, name);
            child->setData(0, Qt::UserRole, path);
            child->setToolTip(0, path);
            child->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
        }
        current = child;
    }
}

QTreeWidgetItem *FolderWizardRemotePath::itemForPath(const QString &path) const
{
    const auto trail = splitRemotePath(path);
    QTreeWidgetItem *item = _ui->folderTreeWidget->topLevelItem(0);
    if (!trail || !item) {
        return nullptr;
    }
    for (const QString &name : *trail) {
        item = findChildByName(item, name);
        if (!item) {
            return nullptr;
        }
    }
    return item;
}

bool FolderWizardRemotePath::selectByPath(const QString &path)
{
    QTreeWidgetItem *item = itemForPath(path);
    if (!item) {
        return false;
    }
    _ui->folderTreeWidget->setCurrentItem(item);
    _ui->folderTreeWidget->scrollToItem(item);
    return true;
}

void FolderWizardRemotePath::setInfo(const QString &message)
{
    _info = message;
    updateMessage();
}

void FolderWizardRemotePath::updateMessage() const
{
    // Serverpfade und lokale Pfade können „<“ oder „&“ enthalten, das Feld zeigt Rich Text
    if (!_validityError.isEmpty()) {
        _ui->warningLabel->setText(QStringLiteral("<b>%1</b>").arg(_validityError.toHtmlEscaped()));
        _ui->warningFrame->show();
    } else if (!_info.isEmpty()) {
        _ui->warningLabel->setText(_info.toHtmlEscaped());
        _ui->warningFrame->show();
    } else {
        _ui->warningLabel->clear();
        _ui->warningFrame->hide();
    }
}
