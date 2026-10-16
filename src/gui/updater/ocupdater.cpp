/*
 * Copyright (C) by Klaas Freitag <freitag@owncloud.com>
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

#include "accessmanager.h"
#include "application.h"
#include "common/utility.h"
#include "common/version.h"
#include "config/appconfig.h"
#include "configfile.h"
#include "mainwindow/modalwrapperwidget.h"
#include "theme.h"


#include "updatedownloadedwidget.h"
#include "updater/newversionavailablewidget.h"
#include "updater/codesignature.h"
#include "updater/ocupdater.h"
#include "updater/updater_private.h"

#include <QObject>
#include <QtCore>
#include <QtGui>
#include <QtNetwork>
#include <QtWidgets>

using namespace std::chrono_literals;

using namespace std::chrono_literals;

namespace OCC {

UpdaterScheduler::UpdaterScheduler(Application *app, QObject *parent)
    : QObject(parent)
    , _skipUpdateCheck(AppConfig().skipUpdateCheck())
{
    connect(&_updateCheckTimer, &QTimer::timeout,
        this, &UpdaterScheduler::slotTimerFired);

    // Note: the sparkle-updater is not an OCUpdater
    if (auto *updater = qobject_cast<OCUpdater *>(Updater::instance())) {
        connect(updater, &OCUpdater::updateAvailableThroughSystem, app,
            [app, updater]() { app->tray()->slotShowTrayMessage(tr("Update available"), updater->statusString()); });

        connect(updater, &OCUpdater::updateDownloaded, this, [app, updater, this]() {
            // prevent dialog from being displayed twice (rather unlikely, but it won't hurt)
            if (_updateDownloadedWidget == nullptr) {
                _updateDownloadedWidget = new UpdateDownloadedWidget(app->mainWindow(), updater->statusString());
                ModalWrapperWidget *wrapper = new ModalWrapperWidget(_updateDownloadedWidget, app->mainWindow());
                ocApp()->showModalWidget(wrapper);

                connect(_updateDownloadedWidget, &UpdateDownloadedWidget::accepted, this, []() { Updater::instance()->applyUpdateAndRestart(); });
            }
        });

        connect(updater, &OCUpdater::retryUpdateCheckLater, this, [this]() {
            qCInfo(lcUpdater) << "Retrying update check in 10 minutes";
            QTimer::singleShot(10min, this, &UpdaterScheduler::slotTimerFired);
        });
    }

    // at startup, do a check in any case.
    QTimer::singleShot(3s, this, &UpdaterScheduler::slotTimerFired);

    ConfigFile cfg;
    auto checkInterval = cfg.updateCheckInterval();
    _updateCheckTimer.start(std::chrono::milliseconds(checkInterval).count());
}

void UpdaterScheduler::slotTimerFired()
{
    ConfigFile cfg;

    // re-set the check interval if it changed in the config file meanwhile
    auto checkInterval = std::chrono::milliseconds(cfg.updateCheckInterval()).count();
    if (checkInterval != _updateCheckTimer.interval()) {
        _updateCheckTimer.setInterval(checkInterval);
        qCInfo(lcUpdater) << "Setting new update check interval " << checkInterval;
    }

    // consider the skipUpdateCheck flag in the config.
    if (_skipUpdateCheck) {
        qCInfo(lcUpdater) << "Skipping update check because of system config";
        return;
    }

    Updater *updater = Updater::instance();
    if (updater) {
        updater->backgroundCheckForUpdate();
    }
}


/* ----------------------------------------------------------------- */

OCUpdater::OCUpdater(const QUrl &url)
    : Updater()
    , _updateUrl(url)
    , _state(Unknown)
    , _accessManager(new AccessManager(this))
    , _timeoutWatchdog(new QTimer(this))
{
}

void OCUpdater::backgroundCheckForUpdate()
{
    int dlState = downloadState();

    // do the real update check depending on the internal state of updater.
    switch (dlState) {
    case Unknown:
    case UpToDate:
    case DownloadFailed:
    case DownloadTimedOut:
        qCInfo(lcUpdater) << "Checking for available update";
        checkForUpdate();
        break;
    case DownloadComplete:
        qCInfo(lcUpdater) << "Update is downloaded, skip new check.";
        break;
    case UpdateOnlyAvailableThroughSystem:
        qCInfo(lcUpdater) << "Update is only available through system, skip check.";
        break;
    }
}

namespace {
struct ParsedVersion
{
    QVersionNumber number;
    QString suffix; // "beta1", "rc2", leer beim Release
};

ParsedVersion parseVersion(const QString &text)
{
    const QString trimmed = text.trimmed();
    qsizetype suffixIndex = 0;
    ParsedVersion parsed{QVersionNumber::fromString(trimmed, &suffixIndex), trimmed.mid(suffixIndex)};
    if (parsed.suffix.startsWith(QLatin1Char('-'))) {
        parsed.suffix.remove(0, 1);
    }
    return parsed;
}

// Ziffernfolgen ohne führende Nullen: erst die Länge, dann der Text, so läuft nichts über
int compareDigits(QStringView a, QStringView b)
{
    while (a.size() > 1 && a.front() == u'0') {
        a = a.mid(1);
    }
    while (b.size() > 1 && b.front() == u'0') {
        b = b.mid(1);
    }
    if (a.size() != b.size()) {
        return a.size() < b.size() ? -1 : 1;
    }
    return a.compare(b);
}

// "beta2" < "beta10" < "rc1": Ziffernfolgen zählen als Zahl, der Rest alphabetisch
int compareSuffixes(const QString &a, const QString &b)
{
    static const QRegularExpression tokenPattern(QStringLiteral("\\d+|\\D+"));
    auto tokensA = tokenPattern.globalMatch(a);
    auto tokensB = tokenPattern.globalMatch(b);
    while (tokensA.hasNext() && tokensB.hasNext()) {
        const QString tokenA = tokensA.next().captured();
        const QString tokenB = tokensB.next().captured();
        const int result = tokenA.front().isDigit() && tokenB.front().isDigit() ? compareDigits(tokenA, tokenB)
                                                                                : tokenA.compare(tokenB, Qt::CaseInsensitive);
        if (result != 0) {
            return result;
        }
    }
    return tokensA.hasNext() ? 1 : (tokensB.hasNext() ? -1 : 0);
}
}

QString OCUpdater::installedVersion()
{
    const QString number = Version::versionWithBuildNumber().toString();
    const QString suffix = Version::suffix();
    return suffix.isEmpty() ? number : QStringLiteral("%1-%2").arg(number, suffix);
}

int OCUpdater::compareVersions(const QString &a, const QString &b)
{
    const ParsedVersion versionA = parseVersion(a);
    const ParsedVersion versionB = parseVersion(b);
    for (int i = 0; i < 3; ++i) {
        if (const int diff = versionA.number.segmentAt(i) - versionB.number.segmentAt(i)) {
            return diff;
        }
    }
    // Die Baunummer ist bei unseren Bauten immer 0: ohne diese Regel bekäme eine Beta-Installation
    // (11.0.0.0-beta1) das finale 11.0.0 nie angeboten.
    if (versionA.suffix.isEmpty() != versionB.suffix.isEmpty()) {
        return versionA.suffix.isEmpty() ? 1 : -1;
    }
    if (const int diff = compareSuffixes(versionA.suffix, versionB.suffix)) {
        return diff;
    }
    return QVersionNumber::compare(versionA.number, versionB.number);
}

bool OCUpdater::isNewerVersion(const QString &available, const QString &installed, const QString &skipped)
{
    if (parseVersion(available).number.isNull() || compareVersions(available, installed) <= 0) {
        return false;
    }
    // leere oder unlesbare Angabe: nichts übersprungen
    return parseVersion(skipped).number.isNull() || compareVersions(available, skipped) > 0;
}

bool OCUpdater::isTrustedDownloadUrl(const QUrl &url)
{
    // Weiterleitungen (objects.githubusercontent.com) folgt Qt nur https -> https
    return url.isValid() && url.scheme() == QLatin1String("https") && url.host() == QLatin1String("github.com")
        && url.userInfo().isEmpty() && url.port(443) == 443
        && url.path().startsWith(QLatin1String("/BWTECH-github/client/releases/download/"))
        && !url.path().contains(QLatin1String("/../")) && url.fileName().endsWith(QLatin1String("-Setup.exe"));
}

QString OCUpdater::statusString() const
{
    QString updateVersion = _updateInfo.versionString();

    switch (downloadState()) {
    case Downloading:
        return tr("Downloading %1. Please wait…").arg(updateVersion);
    case DownloadComplete:
        if (Utility::runningInAppImage()) {
            return tr("%1 installed successfully. Restart the application to finish installing the update.").arg(updateVersion);
        } else {
            return tr("%1 available. Restart application to start the update.").arg(updateVersion);
        }
    case DownloadFailed:
        return tr("Could not download update. Please click <a href='%1'>here</a> to download the update manually.").arg(_updateInfo.web().toHtmlEscaped());
    case DownloadTimedOut:
        return tr("Could not check for new updates.");
    case UpdateOnlyAvailableThroughSystem:
#ifdef Q_OS_LINUX
        // https://docs.appimage.org/packaging-guide/environment-variables.html
        // TODO: update once AppImageUpdate has been implemented
        if (qEnvironmentVariableIsSet("APPIMAGE")) {
            return tr("New %1 available. Please click <a href='%2'>here</a> to download the new AppImage manually.").arg(updateVersion, _updateInfo.web().toHtmlEscaped());
        }
#endif
        return tr("New %1 available. Please use the system's update tool to install it.").arg(updateVersion);
    case CheckingServer:
        return tr("Checking update server…");
    case Unknown:
        return tr("Update status is unknown: Did not check for new updates.");
    case UpToDate:
    // fall through
    default:
        return tr("No updates available. Your installation is at the latest version.");
    }
}

OCUpdater::DownloadState OCUpdater::downloadState() const
{
    return _state;
}

void OCUpdater::setDownloadState(DownloadState state)
{
    auto oldState = _state;
    _state = state;
    Q_EMIT downloadStateChanged();

    // show the notification if the download is complete (on every check)
    // or once for system based updates.
    if (_state == OCUpdater::DownloadComplete) {
        Q_EMIT updateDownloaded();
    }

    if (oldState != OCUpdater::UpdateOnlyAvailableThroughSystem && _state == OCUpdater::UpdateOnlyAvailableThroughSystem) {
        Q_EMIT updateAvailableThroughSystem();
    }
}

QString OCUpdater::availableVersionString() const
{
    return _updateInfo.versionString();
}

void OCUpdater::checkForUpdate()
{
    QNetworkRequest request(_updateUrl);
    // GitHub leitet Release-Downloads per 302 auf release-assets.githubusercontent.com um. Ohne diese Angabe setzt der
    // AccessManager ManualRedirectPolicy: Die Antwort wäre die leere Weiterleitung, der Feed nie lesbar.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = _accessManager->get(request);
    connect(_timeoutWatchdog, &QTimer::timeout, this, &OCUpdater::slotTimedOut);
    _timeoutWatchdog->start(30s);
    connect(reply, &QNetworkReply::finished, this, &OCUpdater::slotVersionInfoArrived);

    setDownloadState(CheckingServer);
}

void OCUpdater::slotOpenUpdateUrl()
{
    QDesktopServices::openUrl(QUrl(_updateInfo.web()));
}

bool OCUpdater::updateSucceeded() const
{
    auto settings = ConfigFile::makeQSettings();

    // geklappt, wenn das Ziel nicht mehr neuer ist als das, was jetzt läuft (auch Beta -> Release)
    return !isNewerVersion(settings.value(updateTargetVersionC).toString(), installedVersion(), {});
}

void OCUpdater::slotVersionInfoArrived()
{
    _timeoutWatchdog->stop();
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        qCWarning(lcUpdater) << "Failed to reach version check url: " << reply->errorString();
        setDownloadState(OCUpdater::Unknown);
        Q_EMIT retryUpdateCheckLater();
        return;
    }

    QString xml = QString::fromUtf8(reply->readAll());

    bool ok;
    _updateInfo = UpdateInfo::parseString(xml, &ok);
    if (ok) {
        versionInfoArrived(_updateInfo);
    } else {
        // eine nicht befolgte Weiterleitung (etwa von https auf http) kommt ohne Fehler, aber mit leerem Inhalt an
        qCWarning(lcUpdater) << "Could not parse update information. HTTP status:" << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()
                             << "redirect target:" << reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
        setDownloadState(OCUpdater::Unknown);
        Q_EMIT retryUpdateCheckLater();
    }
}

void OCUpdater::slotTimedOut()
{
    setDownloadState(DownloadTimedOut);
}

QString OCUpdater::previouslySkippedVersion()
{
    auto settings = ConfigFile::makeQSettings();
    return settings.value(previouslySkippedVersionC).toString();
}

void OCUpdater::setPreviouslySkippedVersion(const QString &previouslySkippedVersion)
{
    auto settings = ConfigFile::makeQSettings();
    settings.setValue(previouslySkippedVersionC, previouslySkippedVersion);
}

////////////////////////////////////////////////////////////////////////

WindowsUpdater::WindowsUpdater(const QUrl &url)
    : OCUpdater(url)
{
}

void WindowsUpdater::slotWriteFile()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    if (_file->isOpen()) {
        _file->write(reply->readAll());
    }
}

void WindowsUpdater::wipeUpdateData()
{
    auto settings = ConfigFile::makeQSettings();
    QString updateFileName = settings.value(updateAvailableC).toString();
    if (!updateFileName.isEmpty())
        QFile::remove(updateFileName);
    settings.remove(updateAvailableC);
    settings.remove(updateTargetVersionC);
    settings.remove(updateTargetVersionStringC);
    settings.remove(autoUpdateAttemptedC);
}

void WindowsUpdater::slotDownloadFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    const QUrl url(reply->url());
    reply->deleteLater();
    _file->close();
    if (reply->error() != QNetworkReply::NoError) {
        setDownloadState(DownloadFailed);
        return;
    }
    const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    if (status != 200) {
        setDownloadState(DownloadFailed);
        return;
    }


    auto settings = ConfigFile::makeQSettings();

    // remove previously downloaded but not used installer
    QFile oldTargetFile(settings.value(updateAvailableC).toString());
    if (oldTargetFile.exists()) {
        oldTargetFile.remove();
    }

    QFile::remove(_targetFile);
    if (!QFile::copy(_file->fileName(), _targetFile) || !verifyDownloadedSetup(_targetFile)) {
        setDownloadState(DownloadFailed);
        return;
    }
    setDownloadState(DownloadComplete);
    qCInfo(lcUpdater) << "Downloaded" << url.toString() << "to" << _targetFile;
    settings.setValue(updateTargetVersionC, updateInfo().version());
    settings.setValue(updateTargetVersionStringC, updateInfo().versionString());
    settings.setValue(updateAvailableC, _targetFile);
}

bool WindowsUpdater::verifyDownloadedSetup(const QString &fileName)
{
    QString error;
    if (CodeSignature::verifyUpdatePackage(fileName, &error)) {
        return true;
    }
    // nie ein Setup starten, das nicht von uns signiert ist
    qCWarning(lcUpdater) << "Rejecting update package" << fileName << ":" << error;
    QFile::remove(fileName);
    return false;
}

void WindowsUpdater::versionInfoArrived(const UpdateInfo &info)
{
    auto settings = ConfigFile::makeQSettings();
    const QString installed = installedVersion();
    const QString previouslySkippedVersion = this->previouslySkippedVersion();
    qCInfo(lcUpdater) << "Version info arrived:"
                      << "Your version:" << installed
                      << "Skipped version:" << previouslySkippedVersion
                      << "Available version:" << info.version()
                      << "Available version string:" << info.versionString()
                      << "Web url:" << info.web()
                      << "Download url:" << info.downloadUrl();
    if (info.version().isEmpty())
    {
        qCInfo(lcUpdater) << "No version information available at the moment";
        setDownloadState(UpToDate);
    } else if (!isNewerVersion(info.version(), installed, previouslySkippedVersion)) {
        qCInfo(lcUpdater) << "Client is on latest version!";
        setDownloadState(UpToDate);
    } else {
        const QString url = info.downloadUrl();
        if (url.isEmpty() || !isTrustedDownloadUrl(QUrl(url))) {
            if (!url.isEmpty()) {
                qCWarning(lcUpdater) << "Untrusted download url, offering manual download only:" << url;
            }
            showNewVersionAvailableWidget(info);
        } else {
            _targetFile = ConfigFile::configPath() + QUrl(url).fileName();
            // eine bereits vorhandene Datei wird erneut geprüft, nicht blind übernommen
            if (QFile::exists(_targetFile) && verifyDownloadedSetup(_targetFile)) {
                setDownloadState(DownloadComplete);
            } else {
                auto request = QNetworkRequest(QUrl(url));
                request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
                QNetworkReply *reply = qnam()->get(request);
                connect(reply, &QIODevice::readyRead, this, &WindowsUpdater::slotWriteFile);
                connect(reply, &QNetworkReply::finished, this, &WindowsUpdater::slotDownloadFinished);
                setDownloadState(Downloading);
                _file.reset(new QTemporaryFile);
                _file->setAutoRemove(true);
                _file->open();
            }
        }
    }
}

void WindowsUpdater::showNewVersionAvailableWidget(const UpdateInfo &info)
{
    // if the version tag is set, there is a newer version.
    QString txt = tr("<p>A new version of the %1 Client is available.</p>"
                     "<p><b>%2</b> is available for download. The installed version is %3.</p>")
                      .arg(Utility::escape(Theme::instance()->appNameGUI()),
                          Utility::escape(info.versionString()), Utility::escape(Version::displayString()));
    auto *widget = new NewVersionAvailableWidget(ocApp()->mainWindow(), txt);

    connect(widget, &NewVersionAvailableWidget::versionSkipped, this, &WindowsUpdater::slotSetPreviouslySkippedVersion);
    connect(widget, &NewVersionAvailableWidget::accepted, this, &WindowsUpdater::slotOpenUpdateUrl);

    ModalWrapperWidget *wrapper = new ModalWrapperWidget(widget, ocApp()->mainWindow());
    ocApp()->showModalWidget(wrapper);
}

void WindowsUpdater::showUpdateErrorDialog(const QString &targetVersion)
{
    QDialog *msgBox = new QDialog;
    msgBox->setAttribute(Qt::WA_DeleteOnClose);

    QIcon infoIcon = msgBox->style()->standardIcon(QStyle::SP_MessageBoxInformation);
    int iconSize = msgBox->style()->pixelMetric(QStyle::PM_MessageBoxIconSize);

    msgBox->setWindowIcon(infoIcon);

    QVBoxLayout *layout = new QVBoxLayout(msgBox);
    QHBoxLayout *hlayout = new QHBoxLayout;
    layout->addLayout(hlayout);

    msgBox->setWindowTitle(tr("Update Failed"));

    QLabel *ico = new QLabel;
    ico->setFixedSize(iconSize, iconSize);
    ico->setPixmap(infoIcon.pixmap(iconSize));
    QLabel *lbl = new QLabel;
    QString txt = tr("<p>A new version of the %1 Client is available but the updating process failed.</p>"
                     "<p><b>%2</b> has been downloaded. The installed version is %3.</p>")
                      .arg(Utility::escape(Theme::instance()->appNameGUI()),
                          Utility::escape(targetVersion), Utility::escape(Version::displayString()));

    lbl->setText(txt);
    lbl->setTextFormat(Qt::RichText);
    lbl->setWordWrap(true);

    hlayout->addWidget(ico);
    hlayout->addWidget(lbl);

    QDialogButtonBox *bb = new QDialogButtonBox;
    QPushButton *skip = bb->addButton(tr("Skip this version"), QDialogButtonBox::ResetRole);
    QPushButton *askagain = bb->addButton(tr("Ask again later"), QDialogButtonBox::ResetRole);
    QPushButton *retry = bb->addButton(tr("Restart and update"), QDialogButtonBox::AcceptRole);
    QPushButton *getupdate = bb->addButton(tr("Update manually"), QDialogButtonBox::AcceptRole);

    connect(skip, &QAbstractButton::clicked, msgBox, &QDialog::reject);
    connect(askagain, &QAbstractButton::clicked, msgBox, &QDialog::reject);
    connect(retry, &QAbstractButton::clicked, msgBox, &QDialog::accept);
    connect(getupdate, &QAbstractButton::clicked, msgBox, &QDialog::accept);

    connect(skip, &QAbstractButton::clicked, this, [this]() {
        wipeUpdateData();
        slotSetPreviouslySkippedVersion();
    });
    // askagain: do nothing
    connect(retry, &QAbstractButton::clicked, this, [this]() {
        applyUpdateAndRestart();
        qApp->quit();
    });
    connect(getupdate, &QAbstractButton::clicked, this, [this]() {
        slotOpenUpdateUrl();
    });

    layout->addWidget(bb);

    msgBox->open();
}

void WindowsUpdater::validateUpdate()
{
    const auto settings = ConfigFile::makeQSettings();
    const QString updateFileName = settings.value(updateAvailableC).toString();
    // has the previous run downloaded an update?
    if (!updateFileName.isEmpty() && QFile(updateFileName).exists()) {
        qCInfo(lcUpdater) << "An updater file is available";
        // did it try to execute the update?
        if (settings.value(autoUpdateAttemptedC, false).toBool()) {
            if (updateSucceeded()) {
                // success: clean up
                qCInfo(lcUpdater) << "The requested update attempt has succeeded"
                                  << installedVersion();
                wipeUpdateData();
            } else {
                // auto update failed. Ask user what to do
                qCInfo(lcUpdater) << "The requested update attempt has failed"
                        << settings.value(updateTargetVersionC).toString();
                showUpdateErrorDialog(settings.value(updateTargetVersionStringC).toString());
            }
        }
    }
}

void WindowsUpdater::slotSetPreviouslySkippedVersion()
{
    setPreviouslySkippedVersion(updateInfo().version());
}

void WindowsUpdater::applyUpdateAndRestart()
{
    Q_ASSERT(downloadState() == DownloadState::DownloadComplete);

    auto settings = ConfigFile::makeQSettings();
    QString updateFile = settings.value(updateAvailableC).toString();
    // unmittelbar vor dem Start erneut prüfen (Datei liegt im Profil und könnte getauscht sein)
    if (!verifyDownloadedSetup(updateFile)) {
        wipeUpdateData();
        setDownloadState(DownloadFailed);
        return;
    }
    settings.setValue(autoUpdateAttemptedC, true);
    settings.sync();
    qCInfo(lcUpdater) << "Running updater" << updateFile;

    // Das NSIS-Setup (admin/win/restart-wrapper.nsi) braucht Adminrechte, beendet
    // den Client selbst und startet ihn im Silent-Modus nicht neu. Darum startet
    // PowerShell das Setup mit /S (UAC-Abfrage), wartet und startet danach den Client.
    auto preparePathForPowershell = [](QString path) {
        path.replace(QLatin1String("'"), QLatin1String("''"));

        return QDir::toNativeSeparators(path);
    };

    const QString command =
        QStringLiteral("&{ try { Start-Process -FilePath '%1' -ArgumentList '/S' -Verb RunAs -Wait } catch {} ; Start-Process -FilePath '%2' }")
            .arg(preparePathForPowershell(updateFile), preparePathForPowershell(QCoreApplication::applicationFilePath()));

    QProcess::startDetached(QStringLiteral("powershell.exe"),
        QStringList{QStringLiteral("-NoProfile"), QStringLiteral("-WindowStyle"), QStringLiteral("Hidden"), QStringLiteral("-Command"), command});
    QTimer::singleShot(0, QApplication::instance(), &QApplication::quit);
}

////////////////////////////////////////////////////////////////////////

namespace {
    QDateTime applicationMTime()
    {
        return QFileInfo(QApplication::applicationFilePath()).lastModified().toUTC();
    }
}

PassiveUpdateNotifier::PassiveUpdateNotifier(const QUrl &url)
    : OCUpdater(url)
    , _initialAppMTime(applicationMTime())
{
}

void PassiveUpdateNotifier::backgroundCheckForUpdate()
{
    if (Utility::isLinux() && !Utility::runningInAppImage()) {
        // a bit of a naive approach, since it doesn't exclude downgrades or forced reinstalls
        // whenever the application binary's mtime changes, we assume it has changed, since users are expected not to call touch on it or otherwise change it
        // manually
        const auto currentMTime = applicationMTime();
        qCDebug(lcUpdater) << "initial mtime:" << _initialAppMTime << "current mtime:" << currentMTime;
        if (currentMTime != _initialAppMTime) {
            qCInfo(lcUpdater) << "Binary mtime changed since application started, requesting restart";
            setDownloadState(DownloadState::DownloadComplete);
            Q_EMIT updateDownloaded();
        }
    }

    OCUpdater::backgroundCheckForUpdate();
}

void PassiveUpdateNotifier::versionInfoArrived(const UpdateInfo &info)
{
    if (!isNewerVersion(info.version(), installedVersion(), {})) {
        qCInfo(lcUpdater) << "Client is on latest version!";
        setDownloadState(UpToDate);
    } else {
        setDownloadState(UpdateOnlyAvailableThroughSystem);
    }
}

} // ns mirall
