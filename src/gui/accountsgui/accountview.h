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

#pragma once

#include "gui/owncloudguilib.h"

#include <QWidget>

class QLabel;
class QProgressBar;

namespace OCC {
class AccountModalWidget;

namespace Ui {
    class AccountView;
}

class FolderMan;

class Account;
class AccountState;
class AccountFoldersView;

/**
 * @brief The AccountView class
 * @ingroup gui
 */
class OWNCLOUDGUI_EXPORT AccountView : public QWidget
{
    Q_OBJECT

public:
    explicit AccountView(QWidget *parent);
    ~AccountView() override;

    void setAccountMenuActions(QList<QAction *> actions);

    // this is primarily used to run an account "modal" widget
    void setTopStackWidget(QWidget *widget);
    void removeStackWidget(QWidget *widget);

    // holding my nose for now - this should not be public, nor should the type be "embedded" in the view's ui.
    // todo: replace the concrete folders view with a placeholder location so the controller can SET the folders view in the
    // account view.
    // open question: would this mess up the squish tests? we'll soon learn the answer
    AccountFoldersView *foldersView();
    void accountSettingUpChanged(bool settingUp);

    void setConnectionIcon(const QIcon &icon);
    void setConnectionLabel(const QString &message, QStringList errors = QStringList());

    /// owncloud.online: Speicherbelegung unter der Statuszeile; totalBytes <= 0 heißt ohne Kontingent (nur „belegt“)
    void setQuota(qint64 usedBytes, qint64 totalBytes);
    /// Farben der Belegungsleiste neu setzen, z. B. nach einem Wechsel des Farbschemas
    void updateQuotaColors();

signals:
    void requestMenuActionUpdate();

protected:
    void showEvent(QShowEvent *ev) override;

private:
    void buildQuotaDisplay();

    Ui::AccountView *_ui;

    QWidget *_quotaWidget = nullptr;
    QProgressBar *_quotaBar = nullptr;
    QLabel *_quotaLabel = nullptr;
    int _quotaPermille = -1; ///< -1: kein Kontingent bekannt, die Leiste ist ausgeblendet
};

} // namespace OCC

Q_DECLARE_METATYPE(OCC::AccountView)
