/*
 * Copyright (C) by Daniel Molkentin <danimo@owncloud.com>
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

#include "networksettings.h"
#include "networkinformation.h"
#include "ui_networksettings.h"

#include "accountmanager.h"
#include "clientproxy.h"
#include "configfile.h"
#include "folderman.h"
#include "theme.h"

#include <QBoxLayout>
#include <QCheckBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QList>
#include <QNetworkProxy>
#include <QSpinBox>
#include <QString>
#include <QtGui/QtEvents>

namespace {
auto proxyPasswordC()
{
    return QStringLiteral("Proxy/Password");
}
}
namespace OCC {

Q_LOGGING_CATEGORY(lcNetworkSettings, "gui.networksettings.gui", QtInfoMsg)

NetworkSettings::NetworkSettings(QWidget *parent)
    : QWidget(parent)
    , _credentialManager(new CredentialManager(this))
    , _ui(new Ui::NetworkSettings)
{
    _ui->setupUi(this);

    setFocusProxy(_ui->pauseSyncWhenMeteredCheckbox);

    _ui->hostLineEdit->setPlaceholderText(tr("Hostname of proxy server"));
    _ui->hostLineEdit->setAccessibleName(tr("Hostname of proxy server"));
    _ui->userLineEdit->setPlaceholderText(tr("Username for proxy server"));
    _ui->userLineEdit->setAccessibleName(tr("Username for proxy server"));
    _ui->passwordLineEdit->setPlaceholderText(tr("Password for proxy server"));
    _ui->passwordLineEdit->setAccessibleName(tr("Password for proxy server"));

    _ui->typeComboBox->addItem(tr("HTTP(S) proxy"), QNetworkProxy::HttpProxy);
    _ui->typeComboBox->addItem(tr("SOCKS5 proxy"), QNetworkProxy::Socks5Proxy);

    _ui->authRequiredcheckBox->setEnabled(true);

    // Explicitly set up the enabled status of the proxy auth widgets to ensure
    // toggling the parent enables/disables the children
    _ui->userLineEdit->setEnabled(true);
    _ui->passwordLineEdit->setEnabled(true);
    _ui->authWidgets->setEnabled(_ui->authRequiredcheckBox->isChecked());
    connect(_ui->authRequiredcheckBox, &QAbstractButton::toggled,
        _ui->authWidgets, &QWidget::setEnabled);

    connect(_ui->manualProxyRadioButton, &QAbstractButton::toggled,
        _ui->manualSettings, &QWidget::setEnabled);
    connect(_ui->manualProxyRadioButton, &QAbstractButton::toggled,
        _ui->typeComboBox, &QWidget::setEnabled);
    connect(_ui->manualProxyRadioButton, &QAbstractButton::toggled,
        this, &NetworkSettings::checkAccountLocalhost);

    loadProxySettings();
    removeBWLimitSettings();
    loadMeteredSettings();
    setupBWLimitUi();
    loadBWLimitSettings();

    // proxy
    connect(_ui->typeComboBox, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this, &NetworkSettings::saveProxySettings);
    connect(_ui->proxyButtonGroup, &QButtonGroup::buttonClicked, this, &NetworkSettings::saveProxySettings);
    connect(_ui->hostLineEdit, &QLineEdit::editingFinished, this, &NetworkSettings::saveProxySettings);
    connect(_ui->userLineEdit, &QLineEdit::editingFinished, this, &NetworkSettings::saveProxySettings);
    connect(_ui->passwordLineEdit, &QLineEdit::editingFinished, this, &NetworkSettings::saveProxySettings);
    connect(_ui->portSpinBox, &QAbstractSpinBox::editingFinished, this, &NetworkSettings::saveProxySettings);
    connect(_ui->authRequiredcheckBox, &QAbstractButton::toggled, this, &NetworkSettings::saveProxySettings);

    // Warn about empty proxy host
    connect(_ui->hostLineEdit, &QLineEdit::textChanged, this, &NetworkSettings::checkEmptyProxyHost);
    checkEmptyProxyHost();
    checkAccountLocalhost();

    connect(_ui->pauseSyncWhenMeteredCheckbox, &QAbstractButton::clicked, this, &NetworkSettings::saveMeteredSettings);
}

NetworkSettings::~NetworkSettings()
{
    delete _ui;
}

void NetworkSettings::setupBWLimitUi()
{
    auto *group = new QGroupBox(tr("Bandwidth"), this);
    auto *grid = new QGridLayout(group);

    // Altwert -1: nur ein Hinweis, gespeichert wird erst eine Änderung des Nutzers an dieser Richtung
    const QString legacyHintText = tr("The automatic limit of an earlier version is no longer supported. Currently not limited.");
    const auto createLegacyHint = [group, &legacyHintText](const QString &objectName) {
        auto *hint = new QLabel(legacyHintText, group);
        hint->setObjectName(objectName);
        hint->setWordWrap(true);
        hint->setVisible(false);
        return hint;
    };

    _uploadLimitCheckBox = new QCheckBox(tr("Limit upload speed to"), group);
    _uploadLimitCheckBox->setObjectName(QStringLiteral("uploadLimitCheckBox"));
    _uploadLimitSpinBox = new QSpinBox(group);
    _uploadLimitSpinBox->setObjectName(QStringLiteral("uploadLimitSpinBox"));
    _uploadLimitSpinBox->setRange(1, 100000);
    _uploadLimitSpinBox->setSuffix(tr(" kB/s"));
    _uploadLimitSpinBox->setValue(100);
    _uploadLegacyHint = createLegacyHint(QStringLiteral("uploadLimitLegacyHint"));
    grid->addWidget(_uploadLimitCheckBox, 0, 0);
    grid->addWidget(_uploadLimitSpinBox, 0, 1);
    grid->addWidget(_uploadLegacyHint, 0, 2);

    _downloadLimitCheckBox = new QCheckBox(tr("Limit download speed to"), group);
    _downloadLimitCheckBox->setObjectName(QStringLiteral("downloadLimitCheckBox"));
    _downloadLimitSpinBox = new QSpinBox(group);
    _downloadLimitSpinBox->setObjectName(QStringLiteral("downloadLimitSpinBox"));
    _downloadLimitSpinBox->setRange(1, 100000);
    _downloadLimitSpinBox->setSuffix(tr(" kB/s"));
    _downloadLimitSpinBox->setValue(100);
    _downloadLegacyHint = createLegacyHint(QStringLiteral("downloadLimitLegacyHint"));
    grid->addWidget(_downloadLimitCheckBox, 1, 0);
    grid->addWidget(_downloadLimitSpinBox, 1, 1);
    grid->addWidget(_downloadLegacyHint, 1, 2);

    grid->setColumnStretch(2, 1);

    connect(_uploadLimitCheckBox, &QAbstractButton::toggled, _uploadLimitSpinBox, &QWidget::setEnabled);
    connect(_downloadLimitCheckBox, &QAbstractButton::toggled, _downloadLimitSpinBox, &QWidget::setEnabled);
    // clicked, not toggled: loadBWLimitSettings() calls setChecked(), and a
    // save fired from that programmatic toggle would overwrite the stored
    // state of the not-yet-loaded other checkbox (same pattern as the
    // metered checkbox above)
    connect(_uploadLimitCheckBox, &QAbstractButton::clicked, this, &NetworkSettings::saveUploadLimitSettings);
    connect(_downloadLimitCheckBox, &QAbstractButton::clicked, this, &NetworkSettings::saveDownloadLimitSettings);
    connect(_uploadLimitSpinBox, &QAbstractSpinBox::editingFinished, this, &NetworkSettings::saveUploadLimitSettings);
    connect(_downloadLimitSpinBox, &QAbstractSpinBox::editingFinished, this, &NetworkSettings::saveDownloadLimitSettings);

    // Place the bandwidth group right below the metered checkbox.
    if (auto *boxLayout = qobject_cast<QBoxLayout *>(layout())) {
        boxLayout->insertWidget(1, group);
    } else if (layout()) {
        layout()->addWidget(group);
    }
}

void NetworkSettings::loadBWLimitSettings()
{
    ConfigFile cfg;
    // 1 = fester Wert, 0 = aus; negativ war in Client 2.x/6.0.3 „automatisch“ und wirkt hier nicht (owncloudpropagator.cpp)
    const int useUploadLimit = cfg.useUploadLimit();
    _uploadLimitCheckBox->setChecked(useUploadLimit == 1);
    if (cfg.uploadLimit() > 0) {
        _uploadLimitSpinBox->setValue(static_cast<int>(cfg.uploadLimit()));
    }
    _uploadLimitSpinBox->setEnabled(_uploadLimitCheckBox->isChecked());
    _uploadLegacyHint->setVisible(useUploadLimit < 0);
    _uploadLimitCheckBox->setAccessibleDescription(useUploadLimit < 0 ? _uploadLegacyHint->text() : QString());

    const int useDownloadLimit = cfg.useDownloadLimit();
    _downloadLimitCheckBox->setChecked(useDownloadLimit == 1);
    if (cfg.downloadLimit() > 0) {
        _downloadLimitSpinBox->setValue(static_cast<int>(cfg.downloadLimit()));
    }
    _downloadLimitSpinBox->setEnabled(_downloadLimitCheckBox->isChecked());
    _downloadLegacyHint->setVisible(useDownloadLimit < 0);
    _downloadLimitCheckBox->setAccessibleDescription(useDownloadLimit < 0 ? _downloadLegacyHint->text() : QString());
}

void NetworkSettings::saveUploadLimitSettings()
{
    ConfigFile cfg;
    cfg.setUseUploadLimit(_uploadLimitCheckBox->isChecked() ? 1 : 0);
    cfg.setUploadLimit(_uploadLimitSpinBox->value());
    // der Nutzer hat entschieden: der Altwert ist ersetzt
    _uploadLegacyHint->setVisible(false);
    _uploadLimitCheckBox->setAccessibleDescription(QString());
}

void NetworkSettings::saveDownloadLimitSettings()
{
    ConfigFile cfg;
    cfg.setUseDownloadLimit(_downloadLimitCheckBox->isChecked() ? 1 : 0);
    cfg.setDownloadLimit(_downloadLimitSpinBox->value());
    _downloadLegacyHint->setVisible(false);
    _downloadLimitCheckBox->setAccessibleDescription(QString());
}


void NetworkSettings::loadProxySettings()
{
    if (Theme::instance()->forceSystemNetworkProxy()) {
        _ui->systemProxyRadioButton->setChecked(true);
        _ui->proxyGroupBox->setEnabled(false);
        return;
    }
    // load current proxy settings
    OCC::ConfigFile cfgFile;
    int type = cfgFile.proxyType();
    switch (type) {
    case QNetworkProxy::NoProxy:
        _ui->noProxyRadioButton->setChecked(true);
        break;
    case QNetworkProxy::DefaultProxy:
        _ui->systemProxyRadioButton->setChecked(true);
        break;
    case QNetworkProxy::Socks5Proxy:
    case QNetworkProxy::HttpProxy:
        _ui->typeComboBox->setCurrentIndex(_ui->typeComboBox->findData(type));
        _ui->manualProxyRadioButton->setChecked(true);
        break;
    default:
        break;
    }

    _ui->hostLineEdit->setText(cfgFile.proxyHostName());
    int port = cfgFile.proxyPort();
    if (port == 0)
        port = 8080;
    _ui->portSpinBox->setValue(port);
    _ui->authRequiredcheckBox->setChecked(cfgFile.proxyNeedsAuth());
    _ui->userLineEdit->setText(cfgFile.proxyUser());

    const QString legacyPasswordKey = QStringLiteral("Proxy/pass");
    const QString legacyPassword = QString::fromUtf8(QByteArray::fromBase64(ConfigFile().makeQSettings().value(legacyPasswordKey).toByteArray()));
    if (!legacyPassword.isEmpty()) {
        qCWarning(lcNetworkSettings) << "Migrating legacy proxy password to keychain";
        ConfigFile().makeQSettings().remove(legacyPasswordKey);
        _credentialManager->set(proxyPasswordC(), legacyPassword);
        _ui->passwordLineEdit->setText(legacyPassword);
        ClientProxy::setupQtProxyFromConfig(legacyPassword);
    } else {
        auto job = _credentialManager->get(proxyPasswordC());
        if (!job) {
            qCWarning(lcNetworkSettings) << "get credentials job is null - most likely the key does not exist in the credentials manager";
            return;
        }
        connect(job, &CredentialJob::finished, this, [job, this] {
            const QString password = job->data().toString();
            _ui->passwordLineEdit->setText(password);
            ClientProxy::setupQtProxyFromConfig(password);
        });
    }
}

void NetworkSettings::removeBWLimitSettings()
{
    // This method should be removed in a future version.
    QSettings settings = ConfigFile::makeQSettings();
    settings.remove("BWLimit");
}

void NetworkSettings::loadMeteredSettings()
{
    if (NetworkInformation::instance()->supports(NetworkInformation::Feature::Metered)) {
        _ui->pauseSyncWhenMeteredCheckbox->setChecked(ConfigFile().pauseSyncWhenMetered());
        return;
    }

    _ui->pauseSyncWhenMeteredCheckbox->setVisible(false);
}

void NetworkSettings::saveProxySettings()
{
    ConfigFile cfgFile;

    checkEmptyProxyHost();
    if (_ui->noProxyRadioButton->isChecked()) {
        cfgFile.setProxyType(QNetworkProxy::NoProxy);
    } else if (_ui->systemProxyRadioButton->isChecked()) {
        cfgFile.setProxyType(QNetworkProxy::DefaultProxy);
    } else if (_ui->manualProxyRadioButton->isChecked()) {
        auto type = static_cast<QNetworkProxy::ProxyType>(_ui->typeComboBox->itemData(_ui->typeComboBox->currentIndex()).toInt());
        if (_ui->hostLineEdit->text().isEmpty()) {
            type = QNetworkProxy::NoProxy;
        }
        _credentialManager->set(proxyPasswordC(), _ui->passwordLineEdit->text());
        cfgFile.setProxyType(type, _ui->hostLineEdit->text(), _ui->portSpinBox->value(), _ui->authRequiredcheckBox->isChecked(), _ui->userLineEdit->text());
    }

    ClientProxy::setupQtProxyFromConfig(_ui->passwordLineEdit->text()); // Refresh the Qt proxy settings as the
    // quota check can happen all the time.

    // ...and set the folders dirty, they refresh their proxy next time they
    // start the sync.
    FolderMan::instance()->setDirtyProxy();

    for (auto account : AccountManager::instance()->accounts()) {
        account->freshConnectionAttempt();
    }
}

void NetworkSettings::saveMeteredSettings()
{
    bool pauseSyncWhenMetered = _ui->pauseSyncWhenMeteredCheckbox->isChecked();
    ConfigFile().setPauseSyncWhenMetered(pauseSyncWhenMetered);
    FolderMan::instance()->scheduler()->setPauseSyncWhenMetered(pauseSyncWhenMetered);
}

void NetworkSettings::checkEmptyProxyHost()
{
    if (_ui->hostLineEdit->isEnabled() && _ui->hostLineEdit->text().isEmpty()) {
        _ui->hostLineEdit->setStyleSheet(QStringLiteral("border: 1px solid red"));
    } else {
        _ui->hostLineEdit->setStyleSheet(QString());
    }
}

void NetworkSettings::showEvent(QShowEvent *event)
{
    if (!event->spontaneous()
        && _ui->manualProxyRadioButton->isChecked()
        && _ui->hostLineEdit->text().isEmpty()) {
        _ui->noProxyRadioButton->setChecked(true);
        checkEmptyProxyHost();
        saveProxySettings();
    }
    checkAccountLocalhost();

    QWidget::showEvent(event);
}


void NetworkSettings::checkAccountLocalhost()
{
    bool visible = false;
    if (_ui->manualProxyRadioButton->isChecked()) {
        // Check if at least one account is using localhost, because Qt proxy settings have no
        // effect for localhost (#7169)
        for (const auto &account : AccountManager::instance()->accounts()) {
            const auto host = account->account()->url().host();
            // Some typical URL for localhost
            if (host == QLatin1String("localhost") || host.startsWith(QLatin1String("127.")) || host == QLatin1String("[::1]"))
                visible = true;
        }
    }
    _ui->labelLocalhost->setVisible(visible);
}


} // namespace OCC
