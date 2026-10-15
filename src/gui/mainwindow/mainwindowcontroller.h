/*
 * Copyright (C) Lisa Reese <lisa.reese@kiteworks.com>
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

#include <QAction>
#include <QObject>
#include <QPointer>

namespace OCC {

class MainWindow;
class AccountsGuiController;
class ColorManager;

class MainWindowController : public QObject
{
    Q_OBJECT
public:
    explicit MainWindowController(MainWindow *window, ColorManager *colorManager, QObject *parent = nullptr);

    // public for now
    void setup();
    // called from tray menu, too. make this a signal/slot connection between the tray menu controller and main window controller
    void onAbout();
    void onHelp();
    // owncloud.online: aus dem Tray-Menü – Hauptfenster zeigen und Einstellungen bzw. Aktivität öffnen
    void showSettings();
    void showActivity();

signals:
    void requestAccountWizard();

protected:
    void addActionToIconUpdates(QAction *action, const QString &iconName);

private:
    QList<QAction *> buildUrlActions();
    QList<QAction *> buildMenuActions();
    void createMoreAction();
    void createSyncErrorsAction();
    void createActivityAction();

    void onSettings();
    void onQuit();

    MainWindow *_window = nullptr;
    ColorManager *_colorManager = nullptr;
    QPointer<QAction> _activityAction;
};
}
