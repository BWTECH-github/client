/*
 *    This software is in the public domain, furnished "as is", without technical
 *    support, and with no warranty, express or implied, as to its usefulness for
 *    any purpose.
 *
 */

// Aktivitäten und Benachrichtigungen vom Server (ServerActivityFetcher) für ein Konto, das sich wie nach „Fertigstellen“
// im Kontoassistenten zum ersten Mal verbindet. v11.0.0-beta1 bis -beta3 brachen genau dort ab: Die Abfragen gehörten
// dem Konto selbst, und AbstractNetworkJob verlangt Q_ASSERT(account != parent) – mit FORCE_ASSERTS ein Absturz.

#include "accountmanager.h"
#include "accountstate.h"
#include "creds/abstractcredentials.h"
#include "libsync/httplogger.h"
#include "models/trayactivitymodels.h"
#include "serveractivityfetcher.h"
#include "testutils/syncenginetestutils.h"
#include "testutils/testutils.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

using namespace OCC;

namespace {

const QUrl serverUrlC(QStringLiteral("http://localhost/owncloud"));
const QString notificationsPathC = QStringLiteral("/owncloud/ocs/v2.php/apps/notifications/api/v1/notifications");

QByteArray toJson(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QByteArray ocsPayload(const QJsonValue &data)
{
    const QJsonObject meta{{QStringLiteral("status"), QStringLiteral("ok")}, {QStringLiteral("statuscode"), 200}};
    return toJson({{QStringLiteral("ocs"), QJsonObject{{QStringLiteral("meta"), meta}, {QStringLiteral("data"), data}}}});
}

// owncloud.online-Server ohne Spaces, mit den OCS-Endpunkten der Benachrichtigungen
QJsonObject serverCapabilities()
{
    const QJsonObject status{{QStringLiteral("installed"), QStringLiteral("1")}, {QStringLiteral("maintenance"), QStringLiteral("0")},
        {QStringLiteral("version"), QStringLiteral("11.0.0.0")}, {QStringLiteral("versionstring"), QStringLiteral("11.0.0")},
        {QStringLiteral("productname"), QStringLiteral("owncloud.online")}};
    return {{QStringLiteral("core"), QJsonObject{{QStringLiteral("status"), status}}},
        {QStringLiteral("dav"), QJsonObject{{QStringLiteral("chunking"), QStringLiteral("1.0")}}},
        {QStringLiteral("checksums"),
            QJsonObject{
                {QStringLiteral("preferredUploadType"), QStringLiteral("SHA1")}, {QStringLiteral("supportedTypes"), QJsonArray{QStringLiteral("SHA1")}}}},
        {QStringLiteral("notifications"),
            QJsonObject{{QStringLiteral("ocs-endpoints"), QJsonArray{QStringLiteral("list"), QStringLiteral("get"), QStringLiteral("delete")}}}}};
}

/**
 * Server, mit dem sich das Konto verbindet (status.php, Capabilities, Benutzer; WebDAV beantwortet der FakeAM), mit einer
 * Aktivität und einer Benachrichtigung, die nach DELETE oder ihrer Aktion erledigt ist. Merkt sich die OCS-Anfragen als
 * „VERB Pfad“.
 */
class ActivityServer
{
public:
    QStringList ocsRequests;
    bool notificationDone = false;

    FakeAM::Override override(QObject *replyParent)
    {
        return [this, replyParent](QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
            const QString path = request.url().path();
            if (path.endsWith(QLatin1String("/status.php"))) {
                const QJsonObject status{{QStringLiteral("installed"), true}, {QStringLiteral("maintenance"), false}, {QStringLiteral("needsDbUpgrade"), false},
                    {QStringLiteral("version"), QStringLiteral("11.0.0.0")}, {QStringLiteral("versionstring"), QStringLiteral("11.0.0")},
                    {QStringLiteral("edition"), QStringLiteral("Community")}, {QStringLiteral("productname"), QStringLiteral("owncloud.online")}};
                return new FakePayloadReply(op, request, toJson(status), replyParent);
            }
            if (!path.contains(QLatin1String("/ocs/"))) {
                return nullptr;
            }
            const QByteArray verb = HttpLogger::requestVerb(op, request);
            ocsRequests.append(QStringLiteral("%1 %2").arg(QString::fromLatin1(verb), path));
            if (path.endsWith(QLatin1String("/cloud/capabilities"))) {
                return new FakePayloadReply(op, request, ocsPayload(QJsonObject{{QStringLiteral("capabilities"), serverCapabilities()}}), replyParent);
            }
            if (path.endsWith(QLatin1String("/cloud/user"))) {
                return new FakePayloadReply(op, request,
                    ocsPayload(QJsonObject{{QStringLiteral("id"), QStringLiteral("admin")}, {QStringLiteral("display-name"), QStringLiteral("Admin")}}),
                    replyParent);
            }
            if (path.endsWith(QLatin1String("/cloud/activity"))) {
                const QJsonObject activity{{QStringLiteral("id"), 12}, {QStringLiteral("subject"), QStringLiteral("You changed Bericht.txt")},
                    {QStringLiteral("file"), QStringLiteral("/Bericht.txt")}, {QStringLiteral("date"), QStringLiteral("2026-10-20T10:00:00+00:00")}};
                return new FakePayloadReply(op, request, ocsPayload(QJsonArray{activity}), replyParent);
            }
            if (path == notificationsPathC && verb == "GET") {
                if (notificationDone) {
                    return new FakePayloadReply(op, request, ocsPayload(QJsonArray()), replyParent);
                }
                const QJsonObject action{{QStringLiteral("label"), QStringLiteral("Accept")},
                    {QStringLiteral("link"), QString(serverUrlC.toString() + QStringLiteral("/ocs/v1.php/apps/files_sharing/api/v1/remote_shares/pending/3"))},
                    {QStringLiteral("type"), QStringLiteral("POST")}, {QStringLiteral("primary"), true}};
                const QJsonObject notification{{QStringLiteral("notification_id"), 7}, {QStringLiteral("app"), QStringLiteral("files_sharing")},
                    {QStringLiteral("subject"), QStringLiteral("Bob shared a folder")},
                    {QStringLiteral("datetime"), QStringLiteral("2026-10-20T09:00:00+00:00")}, {QStringLiteral("actions"), QJsonArray{action}}};
                return new FakePayloadReply(op, request, ocsPayload(QJsonArray{notification}), replyParent);
            }
            // Benachrichtigung erledigen (DELETE) oder ihre Aktion (POST): danach ist sie fort; OCS meldet Erfolg
            if ((verb == "DELETE" && path.startsWith(notificationsPathC + QLatin1Char('/')))
                || (verb == "POST" && path.endsWith(QLatin1String("/remote_shares/pending/3")))) {
                notificationDone = true;
            }
            return new FakePayloadReply(op, request, ocsPayload(QJsonArray()), replyParent);
        };
    }
};

/**
 * Anmeldedaten, die von Anfang an bereit sind, wie nach der Anmeldung im Assistenten. Jede Prüfung des Servers bekommt einen
 * eigenen FakeAM, den der CheckServerJob mit sich löscht; der erste gehört dem Konto.
 */
class ReadyCredentials : public AbstractCredentials
{
public:
    ReadyCredentials(Account *account, ActivityServer *server)
        : AbstractCredentials(account, account)
        , _server(server)
    {
        _wasEverFetched = true;
    }

    OCC::AccessManager *createAccessManager() const override
    {
        auto *am = new FakeAM(nullptr);
        am->setOverride(_server->override(am));
        if (!_handedOut) {
            _handedOut = true;
            am->setParent(const_cast<ReadyCredentials *>(this));
        }
        return am;
    }
    bool ready() const override { return true; }
    void fetchFromKeychain() override { }
    void askFromUser() override { }
    bool stillValid(QNetworkReply *) override { return true; }
    void persist() override { }
    void invalidateToken() override { }
    void forgetSensitiveData() override { }
    bool refreshAccessToken() override { return true; }

private:
    ActivityServer *_server;
    mutable bool _handedOut = false;
};

using AccountStateRaii = TestUtils::TestUtilsPrivate::AccountStateRaii;

// wie NewAccountBuilder nach „Fertigstellen“: das Konto mit den Capabilities aus dem Assistenten zum AccountManager, der gleich
// die Verbindung prüft
AccountStateRaii addConnectingAccount(ActivityServer *server)
{
    std::ignore = TestUtils::folderMan();
    auto *account = new Account(QUuid::createUuid(), QStringLiteral("admin"), serverUrlC);
    account->setCredentials(new ReadyCredentials(account, server));
    account->setCapabilities({account->url(), serverCapabilities().toVariantMap()});
    return {AccountManager::instance()->addAccount(account), &TestUtils::TestUtilsPrivate::accountStateDeleter};
}

} // namespace

class TestServerActivityFetcher : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // Das neue Konto meldet „Verbunden“: Aktivitäten und Benachrichtigungen kommen an, statt dass das Programm abbricht
    void testNewAccountFetchesAfterConnecting()
    {
        // Arrange
        ActivityServer server;
        RecentActivityModel activities(20);
        ServerNotificationModel notifications;
        ServerActivityFetcher fetcher(&activities, &notifications);

        // Act
        const auto accountState = addConnectingAccount(&server);

        // Assert
        QTRY_COMPARE_WITH_TIMEOUT(accountState->state(), AccountState::Connected, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(activities.rowCount(), 1, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(notifications.rowCount(), 1, 10000);
        QCOMPARE(activities.activity(0).subject, QStringLiteral("You changed Bericht.txt"));
        QCOMPARE(activities.activity(0).accountId, accountState->account()->uuid());
        QCOMPARE(notifications.notification(0).id, QStringLiteral("7"));
    }

    // Benachrichtigung erledigen und ihre Aktion ausführen: Beide Anfragen gehen hinaus, die Benachrichtigung verschwindet
    void testDismissAndActionOnConnectedAccount()
    {
        // Arrange
        ActivityServer server;
        RecentActivityModel activities(20);
        ServerNotificationModel notifications;
        ServerActivityFetcher fetcher(&activities, &notifications);
        const auto accountState = addConnectingAccount(&server);
        QTRY_COMPARE_WITH_TIMEOUT(notifications.rowCount(), 1, 10000);
        const QUuid accountId = accountState->account()->uuid();
        const ServerNotification notification = notifications.notification(0);
        QCOMPARE(notification.actions.size(), 1);

        // Act
        fetcher.runNotificationAction(accountId, notification.id, notification.actions.first());
        fetcher.dismissNotification(accountId, notification.id);

        // Assert
        QTRY_VERIFY_WITH_TIMEOUT(
            server.ocsRequests.contains(QStringLiteral("POST /owncloud/ocs/v1.php/apps/files_sharing/api/v1/remote_shares/pending/3")), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(server.ocsRequests.contains(QStringLiteral("DELETE %1/7").arg(notificationsPathC)), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(notifications.rowCount(), 0, 10000);
    }
};

QTEST_MAIN(TestServerActivityFetcher)
#include "testserveractivityfetcher.moc"
