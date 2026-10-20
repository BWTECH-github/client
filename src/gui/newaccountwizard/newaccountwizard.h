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

#include <QWizard>

#include <functional>

namespace OCC {
class WizardPageValidator;

class NewAccountWizard : public QWizard
{
    Q_OBJECT

public:
    /// liefert zur aktuellen Seite die nächste, oder -1 für die Reihenfolge der Seiten (QWizard::nextId())
    using NextIdResolver = std::function<int(int currentId)>;

    NewAccountWizard(QWidget *parent);

    bool validateCurrentPage() override;

    /**
     * Die nächste Seite hängt vom Server ab: nach der Adressseite die Anmeldung im Browser oder die mit Benutzername und
     * Kennwort, danach jeweils die Abschlussseite. Der Controller setzt den Auflöser, sobald die Anmeldeart feststeht.
     */
    int nextId() const override;
    void setNextIdResolver(const NextIdResolver &resolver);

    /** overload of QWizard::addPage which also accepts the associated page validator */
    int addPage(QWizardPage *page, WizardPageValidator *validator);

public Q_SLOTS:

    void reject() override;

private:
    QHash<QWizardPage *, WizardPageValidator *> _pageValidators;
    NextIdResolver _nextIdResolver;
};
}
