/*
 * Copyright (C) by Hannah von Reth <hannah.vonreth@owncloud.com>
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
#include "aboutview.h"
#include "ui_aboutview.h"

#include "gui/guiutility.h"
#include "iconresources.h"
#include "libsync/theme.h"

#ifdef WITH_AUTO_UPDATER
#include "config/appconfig.h"
#include "configfile.h"
#include "updater/ocupdater.h"
#endif

namespace OCC {

AboutView::AboutView(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::AboutView)
{
    ui->setupUi(this);
    ui->aboutText->setText(Theme::instance()->about());
    ui->icon->setPixmap(IconResources::getBrandingIcon(Theme::instance()->aboutIconName()).pixmap(256));
    ui->versionInfo->setText(Theme::instance()->aboutVersions(Theme::VersionFormat::RichText));

    connect(ui->versionInfo, &QTextBrowser::anchorClicked, this, &AboutView::openBrowserFromUrl);
    connect(ui->aboutText, &QLabel::linkActivated, this, &AboutView::openBrowser);

    setupUpdater();
}

void AboutView::setupUpdater()
{
#ifdef WITH_AUTO_UPDATER
    // keine Prüfung per Systemvorgabe (SkipUpdateCheck) oder ohne Feed-Adresse: kein Bereich; Sparkle (macOS) hat eine eigene Oberfläche
    auto *updater = AppConfig().skipUpdateCheck() ? nullptr : qobject_cast<OCUpdater *>(Updater::instance());
    if (!updater) {
        ui->updaterGroup->hide();
        return;
    }

    ui->updateChannel->addItem(tr("Stable"), QStringLiteral("stable"));
    ui->updateChannel->addItem(tr("Beta"), QStringLiteral("beta"));
    ui->updateChannel->setCurrentIndex(ui->updateChannel->findData(updateChannel()));
    // activated statt currentIndexChanged: nur eine Wahl des Nutzers wird gespeichert, nie die Vorgabe beim Füllen
    connect(ui->updateChannel, &QComboBox::activated, this, [this, updater](int index) {
        const QString channel = ui->updateChannel->itemData(index).toString();
        if (channel.isEmpty() || channel == updateChannel()) {
            return;
        }
        ConfigFile().setUpdateChannel(channel);
        updater->setUpdateUrl(Updater::updateUrl());
        updater->checkNow();
    });

    const auto refresh = [this, updater] {
        ui->updateStatusLabel->setText(updater->statusString());
        ui->checkForUpdatesButton->setEnabled(updater->canCheckNow());
        ui->restartButton->setVisible(updater->downloadState() == OCUpdater::DownloadComplete);
    };
    connect(updater, &OCUpdater::downloadStateChanged, this, refresh);
    connect(ui->checkForUpdatesButton, &QPushButton::clicked, updater, &OCUpdater::checkNow);
    connect(ui->restartButton, &QPushButton::clicked, updater, &Updater::applyUpdateAndRestart);
    // „hier herunterladen“ nach einem gescheiterten Download
    connect(ui->updateStatusLabel, &QLabel::linkActivated, this, &AboutView::openBrowser);
    refresh();
#else
    ui->updaterGroup->hide();
#endif
}

AboutView::~AboutView()
{
    delete ui;
}

void AboutView::openBrowser(const QString &s)
{
    Utility::openBrowser(QUrl(s), this);
}

void AboutView::openBrowserFromUrl(const QUrl &s)
{
    return openBrowser(s.toString());
}

} // OCC namespace
