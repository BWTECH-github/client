/*
 * Copyright (C) by BW-Tech GmbH for owncloud.online.
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

#include <QtTest>

#include "configfile.h"
#include "gui/networksettings.h"

#include <QCheckBox>
#include <QLabel>

using namespace OCC;

class TestNetworkSettings : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // Altwert -1 („automatisch“ aus Client 2.x/6.0.3): darf nicht still zu 0 werden, wenn nur die andere Richtung geändert wird
    void testLegacyAutomaticLimitIsKept()
    {
        // Arrange
        ConfigFile cfg;
        cfg.setUseUploadLimit(-1);
        cfg.setUploadLimit(80);
        cfg.setUseDownloadLimit(1);
        cfg.setDownloadLimit(500);

        NetworkSettings settings;
        auto *uploadCheck = settings.findChild<QCheckBox *>(QStringLiteral("uploadLimitCheckBox"));
        auto *downloadCheck = settings.findChild<QCheckBox *>(QStringLiteral("downloadLimitCheckBox"));
        auto *uploadHint = settings.findChild<QLabel *>(QStringLiteral("uploadLimitLegacyHint"));
        auto *downloadHint = settings.findChild<QLabel *>(QStringLiteral("downloadLimitLegacyHint"));
        QVERIFY(uploadCheck && downloadCheck && uploadHint && downloadHint);
        // „automatisch“ wirkt nicht mehr: nicht als Begrenzung zeigen, aber darauf hinweisen
        QVERIFY(!uploadCheck->isChecked());
        QVERIFY(!uploadHint->isHidden());
        QVERIFY(downloadCheck->isChecked());
        QVERIFY(downloadHint->isHidden());

        // Act: nur das Herunterladen ändern
        downloadCheck->click();

        // Assert: der Altwert des Hochladens bleibt stehen
        QCOMPARE(cfg.useUploadLimit(), -1);
        QCOMPARE(cfg.uploadLimit(), 80);
        QCOMPARE(cfg.useDownloadLimit(), 0);
        QVERIFY(!uploadHint->isHidden());

        // Act: jetzt entscheidet der Nutzer über das Hochladen
        uploadCheck->click();

        // Assert
        QCOMPARE(cfg.useUploadLimit(), 1);
        QCOMPARE(cfg.uploadLimit(), 80);
        QVERIFY(uploadHint->isHidden());
        QCOMPARE(cfg.useDownloadLimit(), 0);
    }
};

QTEST_MAIN(TestNetworkSettings)
#include "testnetworksettings.moc"
