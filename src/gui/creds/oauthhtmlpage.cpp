//
// Created by deepdiver on 27/08/2025.
//

#include "oauthhtmlpage.h"
#include "resources/template.h"

#include <QMap>

namespace {
// Die Texte enthalten Angaben des Servers (Fehlerbeschreibung, Benutzername). renderTemplate() ersetzt Platzhalter „@{…}“ auch in
// den eingesetzten Werten: Ein „@{MESSAGE}“ darin liefe endlos, ein unbekannter Name beendete mit FORCE_ASSERTS (Beta) das
// Programm. Als Zeichenreferenz zeigt der Browser dasselbe „@“.
QString withoutPlaceholders(QString html)
{
    return html.replace(QStringLiteral("@{"), QStringLiteral("&#64;{"));
}
}

namespace OCC {

QString OAuthHtmlPage::buildPage(bool success, const QString &title, const QString &message)
{
    auto templateFile = success ? QStringLiteral(":/client/resources/oauth/success.html.in") : QStringLiteral(":/client/resources/oauth/error.html.in");
    QMap<QString, QString> values = {
        {QStringLiteral("TITLE"), withoutPlaceholders(title)},
        {QStringLiteral("MESSAGE"), withoutPlaceholders(message)},
    };
    return Resources::Template::renderTemplateFromFile(templateFile, values);
}

} // OCC