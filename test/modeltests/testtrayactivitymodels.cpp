/*
 *    This software is in the public domain, furnished "as is", without technical
 *    support, and with no warranty, express or implied, as to its usefulness for
 *    any purpose.
 *
 */

#include "gui/models/trayactivitymodels.h"
#include "gui/serveractivityfetcher.h"
#include "gui/traymenucontroller.h"

#include <QAbstractItemModelTester>
#include <QJsonDocument>
#include <QTest>

namespace OCC {

namespace {
    QDateTime utc(const char *iso)
    {
        return QDateTime::fromString(QString::fromLatin1(iso), Qt::ISODate);
    }

    TrayActivity localActivity(const QUuid &accountId, const QString &path, const QDateTime &time)
    {
        TrayActivity activity;
        activity.accountId = accountId;
        activity.localPath = path;
        activity.relativePath = path;
        activity.subject = path;
        activity.detail = QStringLiteral("Downloaded");
        activity.time = time;
        return activity;
    }

    template <typename Model>
    bool isSortedNewestFirst(const Model *model, int role)
    {
        for (int row = 1; row < model->rowCount(); ++row) {
            if (model->index(row - 1, 0).data(role).toDateTime() < model->index(row, 0).data(role).toDateTime()) {
                return false;
            }
        }
        return true;
    }

    QJsonArray jsonArray(const char *json)
    {
        return QJsonDocument::fromJson(QByteArray(json)).array();
    }
}

class TestTrayActivityModels : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testRecentLocalActivities()
    {
        auto *model = new RecentActivityModel(3, this);
        new QAbstractItemModelTester(model, QAbstractItemModelTester::FailureReportingMode::QtTest, this);
        const QUuid account = QUuid::createUuid();

        for (int i = 0; i < 4; ++i) {
            model->addLocalActivity(localActivity(account, QStringLiteral("/sync/file%1").arg(i), utc("2026-10-15T10:00:00Z").addSecs(i)));
        }
        // die älteste fällt über der Grenze weg, die neueste steht oben
        QCOMPARE(model->rowCount(), 3);
        QCOMPARE(model->index(0, 0).data(RecentActivityModel::LocalPathRole).toString(), QStringLiteral("/sync/file3"));
        QCOMPARE(model->index(2, 0).data(RecentActivityModel::LocalPathRole).toString(), QStringLiteral("/sync/file1"));

        // dieselbe Datei erscheint nur einmal, ihr neuer Eintrag steht oben
        model->addLocalActivity(localActivity(account, QStringLiteral("/sync/file1"), utc("2026-10-15T10:00:10Z")));
        QCOMPARE(model->rowCount(), 3);
        QCOMPARE(model->index(0, 0).data(RecentActivityModel::LocalPathRole).toString(), QStringLiteral("/sync/file1"));
        QCOMPARE(model->index(0, 0).data(Qt::DisplayRole).toString(), QStringLiteral("/sync/file1"));
        QCOMPARE(model->index(0, 0).data(RecentActivityModel::SourceRole).toInt(), static_cast<int>(TrayActivity::Source::Local));
        QVERIFY(isSortedNewestFirst(model, RecentActivityModel::TimeRole));
    }

    void testRecentServerActivities()
    {
        auto *model = new RecentActivityModel(5, this);
        new QAbstractItemModelTester(model, QAbstractItemModelTester::FailureReportingMode::QtTest, this);
        const QUuid localAccount = QUuid::createUuid();
        const QUuid serverAccount = QUuid::createUuid();

        model->addLocalActivity(localActivity(localAccount, QStringLiteral("/sync/a"), utc("2026-10-15T10:00:00Z")));
        model->addLocalActivity(localActivity(localAccount, QStringLiteral("/sync/b"), utc("2026-10-15T12:00:00Z")));

        TrayActivity early;
        early.subject = QStringLiteral("early");
        early.time = utc("2026-10-15T09:00:00Z");
        TrayActivity middle;
        middle.subject = QStringLiteral("middle");
        middle.time = utc("2026-10-15T11:00:00Z");
        model->setServerActivities(serverAccount, {early, middle});
        QCOMPARE(model->rowCount(), 4);
        QVERIFY(isSortedNewestFirst(model, RecentActivityModel::TimeRole));
        QCOMPARE(model->activity(1).subject, QStringLiteral("middle"));
        QCOMPARE(model->activity(1).source, TrayActivity::Source::Server);
        QCOMPARE(model->activity(1).accountId, serverAccount);

        // ersetzt nur die Server-Einträge dieses Kontos
        model->setServerActivities(serverAccount, {middle});
        QCOMPARE(model->rowCount(), 3);

        // lokale Einträge überstehen das Leeren der Server-Liste
        model->setServerActivities(serverAccount, {});
        QCOMPARE(model->rowCount(), 2);

        model->setServerActivities(serverAccount, {early});
        model->removeAccount(localAccount);
        QCOMPARE(model->rowCount(), 1);
        QCOMPARE(model->activity(0).subject, QStringLiteral("early"));
    }

    void testParseServerActivities()
    {
        const QUuid account = QUuid::createUuid();
        const auto activities = RecentActivityModel::parseServerActivities(jsonArray(R"([
            {"id": 12, "subject": "You changed a.txt", "message": "", "file": "/a.txt",
             "link": "https://cloud.example.com/index.php/apps/files/?dir=/", "date": "2026-10-15T10:00:00+02:00"},
            {"id": "13", "subject": "", "message": "", "file": "/b.txt", "link": "", "date": "garbage"},
            {"id": 14, "subject": "", "file": ""}
        ])"),
            account);
        QCOMPARE(activities.size(), 2);
        QCOMPARE(activities[0].id, QStringLiteral("12"));
        QCOMPARE(activities[0].subject, QStringLiteral("You changed a.txt"));
        QCOMPARE(activities[0].time, utc("2026-10-15T08:00:00Z"));
        QCOMPARE(activities[0].link, QUrl(QStringLiteral("https://cloud.example.com/index.php/apps/files/?dir=/")));
        QCOMPARE(activities[0].source, TrayActivity::Source::Server);
        QCOMPARE(activities[0].accountId, account);
        // ohne Betreff zählt der Dateiname, eine unlesbare Zeit sortiert ans Ende
        QCOMPARE(activities[1].subject, QStringLiteral("/b.txt"));
        QCOMPARE(activities[1].time, QDateTime::fromSecsSinceEpoch(0));
    }

    void testServerNotifications()
    {
        auto *model = new ServerNotificationModel(this);
        new QAbstractItemModelTester(model, QAbstractItemModelTester::FailureReportingMode::QtTest, this);
        const QUuid first = QUuid::createUuid();
        const QUuid second = QUuid::createUuid();

        const auto parsed = ServerNotificationModel::parseNotifications(jsonArray(R"([
            {"notification_id": 5, "app": "files_sharing", "subject": "Bob shared a folder", "message": "Accept it?",
             "link": "https://cloud.example.com/index.php/apps/files", "datetime": "2026-10-15T09:00:00+00:00",
             "actions": [
                {"label": "Accept%20share", "link": "https://cloud.example.com/ocs/v1.php/apps/files_sharing/api/v1/remote_shares/pending/3", "type": "post", "primary": true},
                {"label": "", "link": "https://cloud.example.com/", "type": "DELETE", "primary": false}
             ]},
            {"notification_id": "6", "subject": "", "message": "Only a message", "datetime": "2026-10-15T11:00:00+00:00", "actions": []},
            {"subject": "no id"}
        ])"),
            first);
        QCOMPARE(parsed.size(), 2);
        QCOMPARE(parsed[0].id, QStringLiteral("5"));
        QCOMPARE(parsed[0].actions.size(), 1);
        QCOMPARE(parsed[0].actions[0].label, QStringLiteral("Accept share"));
        QCOMPARE(parsed[0].actions[0].verb, QByteArrayLiteral("POST"));
        QVERIFY(parsed[0].actions[0].primary);
        // ohne Betreff rückt die Nachricht nach
        QCOMPARE(parsed[1].subject, QStringLiteral("Only a message"));
        QVERIFY(parsed[1].message.isEmpty());

        // beim ersten Mal ist alles neu, danach nur, was dazukommt
        QCOMPARE(model->setNotifications(first, parsed).size(), 2);
        QCOMPARE(model->rowCount(), 2);
        QVERIFY(isSortedNewestFirst(model, ServerNotificationModel::TimeRole));
        QCOMPARE(model->index(0, 0).data(ServerNotificationModel::IdRole).toString(), QStringLiteral("6"));
        QCOMPARE(model->setNotifications(first, parsed).size(), 0);

        ServerNotification later;
        later.id = QStringLiteral("7");
        later.subject = QStringLiteral("later");
        later.time = utc("2026-10-15T12:00:00Z");
        auto withLater = parsed;
        withLater.append(later);
        const auto fresh = model->setNotifications(first, withLater);
        QCOMPARE(fresh.size(), 1);
        QCOMPARE(fresh[0].id, QStringLiteral("7"));
        QCOMPARE(model->rowCount(), 3);

        QVERIFY(model->removeNotification(first, QStringLiteral("5")));
        QVERIFY(!model->removeNotification(first, QStringLiteral("5")));
        QVERIFY(!model->removeNotification(second, QStringLiteral("6")));
        QCOMPARE(model->rowCount(), 2);

        model->setNotifications(second, {later});
        QCOMPARE(model->rowCount(), 3);
        model->removeAccount(first);
        QCOMPARE(model->rowCount(), 1);
        QCOMPARE(model->notification(0).accountId, second);
    }

    void testApiPathForLink_data()
    {
        QTest::addColumn<QString>("accountUrl");
        QTest::addColumn<QString>("link");
        QTest::addColumn<bool>("allowed");
        QTest::addColumn<QString>("path");

        const QString root = QStringLiteral("https://cloud.example.com/");
        const QString sub = QStringLiteral("https://example.com/owncloud");
        QTest::newRow("same server") << root << QStringLiteral("https://cloud.example.com/ocs/v2.php/apps/notifications/api/v1/notifications/5?x=1") << true
                                     << QStringLiteral("ocs/v2.php/apps/notifications/api/v1/notifications/5");
        QTest::newRow("host case") << root << QStringLiteral("https://CLOUD.example.com/ocs/x") << true << QStringLiteral("ocs/x");
        QTest::newRow("explicit default port") << root << QStringLiteral("https://cloud.example.com:443/ocs/x") << true << QStringLiteral("ocs/x");
        QTest::newRow("relative") << root << QStringLiteral("ocs/v2.php/x") << true << QStringLiteral("ocs/v2.php/x");
        QTest::newRow("sub path") << sub << QStringLiteral("https://example.com/owncloud/ocs/v1.php/apps/files_sharing/api/v1/remote_shares/pending/3")
                                  << true << QStringLiteral("ocs/v1.php/apps/files_sharing/api/v1/remote_shares/pending/3");
        QTest::newRow("foreign host") << root << QStringLiteral("https://evil.example.org/ocs/x") << false << QString();
        QTest::newRow("host suffix") << root << QStringLiteral("https://cloud.example.com.evil.org/ocs/x") << false << QString();
        QTest::newRow("other port") << root << QStringLiteral("https://cloud.example.com:8443/ocs/x") << false << QString();
        QTest::newRow("downgrade to http") << root << QStringLiteral("http://cloud.example.com/ocs/x") << false << QString();
        QTest::newRow("user info") << root << QStringLiteral("https://user@cloud.example.com/ocs/x") << false << QString();
        QTest::newRow("outside sub path") << sub << QStringLiteral("https://example.com/other/ocs/x") << false << QString();
        QTest::newRow("sibling prefix") << sub << QStringLiteral("https://example.com/owncloud-evil/ocs/x") << false << QString();
        QTest::newRow("traversal") << sub << QStringLiteral("https://example.com/owncloud/../other/x") << false << QString();
        QTest::newRow("server root only") << root << QStringLiteral("https://cloud.example.com/") << false << QString();
        QTest::newRow("file scheme") << QStringLiteral("file:///srv/") << QStringLiteral("file:///srv/ocs/x") << false << QString();
    }

    void testApiPathForLink()
    {
        QFETCH(QString, accountUrl);
        QFETCH(QString, link);
        QFETCH(bool, allowed);
        QFETCH(QString, path);

        QString resultPath;
        QList<QPair<QString, QString>> query;
        QCOMPARE(ServerActivityFetcher::apiPathForLink(QUrl(accountUrl), QUrl(link), &resultPath, &query), allowed);
        if (allowed) {
            QCOMPARE(resultPath, path);
        }
    }

    void testApiPathForLinkQuery()
    {
        QString path;
        QList<QPair<QString, QString>> query;
        QVERIFY(ServerActivityFetcher::apiPathForLink(
            QUrl(QStringLiteral("https://cloud.example.com")), QUrl(QStringLiteral("https://cloud.example.com/ocs/x?a=1&b=two%20words")), &path, &query));
        QCOMPARE(path, QStringLiteral("ocs/x"));
        QCOMPARE(query.size(), 2);
        QCOMPARE(query[0], qMakePair(QStringLiteral("a"), QStringLiteral("1")));
        QCOMPARE(query[1], qMakePair(QStringLiteral("b"), QStringLiteral("two words")));
    }

    void testIsWebLink()
    {
        QVERIFY(ServerActivityFetcher::isWebLink(QUrl(QStringLiteral("https://cloud.example.com/index.php"))));
        QVERIFY(ServerActivityFetcher::isWebLink(QUrl(QStringLiteral("http://cloud.example.com/"))));
        QVERIFY(!ServerActivityFetcher::isWebLink(QUrl(QStringLiteral("file:///etc/passwd"))));
        QVERIFY(!ServerActivityFetcher::isWebLink(QUrl(QStringLiteral("javascript:alert(1)"))));
        QVERIFY(!ServerActivityFetcher::isWebLink(QUrl(QStringLiteral("smb://server/share"))));
        QVERIFY(!ServerActivityFetcher::isWebLink(QUrl()));
    }

    void testPauseEnd()
    {
        const auto now = utc("2026-10-15T15:30:00Z");
        QCOMPARE(TrayMenuController::pauseEnd(TrayMenuController::PauseDuration::OneHour, now), utc("2026-10-15T16:30:00Z"));
        QCOMPARE(TrayMenuController::pauseEnd(TrayMenuController::PauseDuration::TwoHours, now), utc("2026-10-15T17:30:00Z"));
        QCOMPARE(TrayMenuController::pauseEnd(TrayMenuController::PauseDuration::FourHours, now), utc("2026-10-15T19:30:00Z"));
        QCOMPARE(TrayMenuController::pauseEnd(TrayMenuController::PauseDuration::UntilTomorrow, now), utc("2026-10-16T00:00:00Z"));
        // kurz vor Mitternacht: trotzdem bis zum Beginn des nächsten Tages
        QCOMPARE(TrayMenuController::pauseEnd(TrayMenuController::PauseDuration::UntilTomorrow, utc("2026-12-31T23:59:00Z")), utc("2027-01-01T00:00:00Z"));
        QVERIFY(!TrayMenuController::pauseEnd(TrayMenuController::PauseDuration::UntilResumed, now).isValid());
    }

    void testMenuText()
    {
        QCOMPARE(TrayMenuController::menuText(QStringLiteral("Tom & Jerry")), QStringLiteral("Tom && Jerry"));
        QCOMPARE(TrayMenuController::menuText(QStringLiteral("  line one\nline\ttwo  ")), QStringLiteral("line one line two"));

        const QString elided = TrayMenuController::menuText(QString(100, QLatin1Char('x')), 21);
        QCOMPARE(elided.size(), 21);
        QCOMPARE(elided.at(10), QChar(0x2026));

        // ein Emoji an der Schnittstelle wird nicht halbiert
        const QString emoji = QString::fromUtf8("\xF0\x9F\x98\x80");
        const QString withEmoji = TrayMenuController::menuText(QString(9, QLatin1Char('a')) + emoji + QString(20, QLatin1Char('b')), 21);
        QCOMPARE(QString::fromUtf8(withEmoji.toUtf8()), withEmoji);
        QVERIFY(withEmoji.contains(QChar(0x2026)));

        // gekürzt wird vor dem Maskieren: aus & wird nie ein halbes &&
        const QString ampersands = TrayMenuController::menuText(QString(50, QLatin1Char('&')), 21);
        QCOMPARE(ampersands.count(QLatin1Char('&')) % 2, 0);
    }
};
}

QTEST_GUILESS_MAIN(OCC::TestTrayActivityModels)
#include "testtrayactivitymodels.moc"
