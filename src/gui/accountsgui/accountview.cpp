/*
 * Copyright (C) by Lisa Reese <lisa.reese@kiteworks.com>
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


#include "accountview.h"
#include "ui_accountview.h"

#include "common/utility.h"
#include "libsync/theme.h"
#include "mainwindow/colormanager.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QProgressBar>
#include <QVBoxLayout>

namespace OCC {

AccountView::AccountView(QWidget *parent)
    : QWidget(parent)
    , _ui(new Ui::AccountView)
{
    _ui->setupUi(this);
    _ui->connectionStatusLabel->clear();

    connect(_ui->stackedWidget, &QStackedWidget::currentChanged, this,
        [this] { _ui->manageAccountButton->setEnabled(_ui->stackedWidget->currentWidget() == _ui->accountFoldersView); });
    _ui->stackedWidget->setCurrentWidget(_ui->accountFoldersView);

    buildQuotaDisplay();
}

AccountView::~AccountView()
{
    delete _ui;
}

void AccountView::setAccountMenuActions(QList<QAction *> actions)
{
    QMenu *menu = _ui->manageAccountButton->menu();
    if (!menu) {
        menu = new QMenu(this);
        menu->setObjectName("manageAccountMenu");
        menu->setAccessibleName(tr("Account options menu"));
        connect(menu, &QMenu::aboutToShow, this, &AccountView::requestMenuActionUpdate);
    } else {
        menu->clear();
    }

    menu->addActions(actions);
    if (_ui->manageAccountButton->menu() == nullptr)
        _ui->manageAccountButton->setMenu(menu);
}

void AccountView::setTopStackWidget(QWidget *widget)
{
    // should not contain this widget already
    Q_ASSERT(_ui->stackedWidget->indexOf(widget) < 0);

    _ui->stackedWidget->addWidget(widget);
    _ui->stackedWidget->setCurrentWidget(widget);
}

void AccountView::removeStackWidget(QWidget *widget)
{
    _ui->stackedWidget->removeWidget(widget);
}

AccountFoldersView *AccountView::foldersView()
{
    return _ui->accountFoldersView;
}

void AccountView::accountSettingUpChanged(bool settingUp)
{
    if (settingUp) {
        _ui->spinner->startAnimation();
        _ui->stackedWidget->setCurrentWidget(_ui->loadingPage);
    } else {
        _ui->spinner->stopAnimation();
        _ui->stackedWidget->setCurrentWidget(_ui->accountFoldersView);
    }
}

void AccountView::showEvent(QShowEvent *ev)
{
    Q_UNUSED(ev);
    _ui->manageAccountButton->setFocus();
}

void AccountView::setConnectionIcon(const QIcon &icon)
{
    // I really see no point in these but they existed previously...eval usefulness later
    // oh wait...the "warning label" is actually the name of the STATUS ICON?!?!
    // if so this makes more sense. for goodness sake, let's rename that item asap :D
    _ui->warningLabel->setVisible(!icon.isNull());
    if (!icon.isNull()) {
        _ui->warningLabel->setPixmap(icon.pixmap(_ui->warningLabel->size()));
    }
}

void AccountView::setConnectionLabel(const QString &message, QStringList errors)
{

    _ui->accountStatus->setVisible(!message.isEmpty());

    if (errors.isEmpty()) {
        _ui->connectionStatusLabel->setText(message);
        _ui->connectionStatusLabel->setToolTip(QString());
    } else {
        errors.prepend(message);
        const QString msg = errors.join(QLatin1String("\n"));
        _ui->connectionStatusLabel->setText(msg);
        _ui->connectionStatusLabel->setToolTip(QString());
    }
}

void AccountView::buildQuotaDisplay()
{
    // owncloud.online: schmale Belegungsleiste direkt unter der Statuszeile, gespeist von QuotaInfo über den
    // AccountViewController; bis zur ersten Antwort des Servers ausgeblendet
    _quotaWidget = new QWidget(this);
    auto *quotaLayout = new QHBoxLayout(_quotaWidget);
    quotaLayout->setContentsMargins(0, 0, 0, 4);
    _quotaBar = new QProgressBar(_quotaWidget);
    _quotaBar->setObjectName(QStringLiteral("accountQuotaBar"));
    _quotaBar->setRange(0, 1000);
    _quotaBar->setTextVisible(false);
    _quotaBar->setFixedHeight(8);
    _quotaBar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    _quotaLabel = new QLabel(_quotaWidget);
    _quotaLabel->setObjectName(QStringLiteral("accountQuotaLabel"));
    quotaLayout->addWidget(_quotaBar, 1);
    quotaLayout->addWidget(_quotaLabel, 0);
    if (auto *mainLayout = qobject_cast<QVBoxLayout *>(layout()))
        mainLayout->insertWidget(1, _quotaWidget);
    _quotaWidget->setVisible(false);
}

void AccountView::setQuota(qint64 usedBytes, qint64 totalBytes)
{
    _quotaWidget->setVisible(true);
    if (totalBytes > 0) {
        // Gleitkomma: usedBytes * 1000 könnte bei großen (vom Server gelieferten) Werten qint64 überlaufen
        _quotaPermille = int(qBound(0.0, double(usedBytes) * 1000.0 / double(totalBytes), 1000.0));
        _quotaBar->setValue(_quotaPermille);
        _quotaBar->setVisible(true);
        //: storage quota, %1 and %2 are sizes like "11.2 GB"
        _quotaLabel->setText(tr("%1 of %2 used").arg(Utility::octetsToString(usedBytes), Utility::octetsToString(totalBytes)));
    } else {
        _quotaPermille = -1;
        _quotaBar->setVisible(false);
        //: storage quota without a server-side limit, %1 is a size like "11.2 GB"
        _quotaLabel->setText(tr("%1 used").arg(Utility::octetsToString(usedBytes)));
    }
    updateQuotaColors();
}

void AccountView::updateQuotaColors()
{
    if (_quotaPermille < 0)
        return;
    // Markenakzent, ab 90 % Belegung die Fehlerfarbe - beide aus dem Theme und passend zum Farbschema
    const bool dark = ColorManager::isDarkPalette();
    const bool critical = _quotaPermille >= 900;
    QColor chunk = critical ? Theme::instance()->errorColor(dark) : Theme::instance()->accentColor(dark);
    if (!chunk.isValid())
        chunk = palette().color(QPalette::Highlight);
    // die halbtransparente Spur passt auf hellen und dunklen Hintergrund
    _quotaBar->setStyleSheet(QStringLiteral("QProgressBar { background: rgba(127,127,127,60); border: none; border-radius: 4px; }"
                                            "QProgressBar::chunk { background: %1; border-radius: 4px; }")
            .arg(chunk.name()));
}

} // namespace OCC
