/*
 *    This software is in the public domain, furnished "as is", without technical
 *    support, and with no warranty, express or implied, as to its usefulness for
 *    any purpose.
 *
 */

#include "account.h"
#include "graphapi/spacesmanager.h"
#include "testutils/syncenginetestutils.h"
#include "testutils/testutils.h"

#include <QSignalSpy>
#include <QTest>

#include <memory>

using namespace OCC;

namespace {
const QString drivesPathC = QStringLiteral("/graph/v1.0/me/drives");

// Capabilities wie TestUtils::testCapabilities(), wahlweise ohne "spaces" wie bei oC10
Capabilities capabilities(const QUrl &url, bool spaces)
{
    auto raw = TestUtils::testCapabilities();
    if (!spaces) {
        raw.remove(QStringLiteral("spaces"));
    }
    return {url, raw};
}
}

class TestSpacesManager : public QObject
{
    Q_OBJECT

    std::unique_ptr<Account> _account;
    int _drivesRequests = 0;

    // Konto wie createDummyAccount(), aber noch ohne Capabilities und ohne AccountManager
    void createAccount()
    {
        _drivesRequests = 0;
        _account = std::make_unique<Account>(QUuid::createUuid(), QStringLiteral("admin"), QUrl(QStringLiteral("http://localhost/owncloud")));
        auto *fakeAm = new FakeAM({}, nullptr);
        fakeAm->setOverride([this](QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
            if (request.url().path().endsWith(drivesPathC)) {
                ++_drivesRequests;
                return new FakePayloadReply(op, request, QByteArrayLiteral("{\"value\":[]}"), this);
            }
            return nullptr;
        });
        _account->setCredentials(new FakeCredentials(_account.get(), fakeAm, _account.get()));
    }

private Q_SLOTS:
    void cleanup() { _account.reset(); }

    void testNoDrivesRequestAgainstOc10()
    {
        createAccount();
        QSignalSpy ready(_account->spacesManager(), &GraphApi::SpacesManager::ready);
        _account->setCapabilities(capabilities(_account->url(), false));
        Q_EMIT _account->credentialsFetched();
        QCOMPARE(ready.count(), 1);
        // Zeit für eine versehentlich gestartete Anfrage
        QTest::qWait(100);
        QCOMPARE(_drivesRequests, 0);
    }

    void testNoDrivesRequestBeforeCapabilities()
    {
        createAccount();
        Q_EMIT _account->credentialsFetched();
        QTest::qWait(100);
        QCOMPARE(_drivesRequests, 0);
        QVERIFY(!_account->spacesManager()->isReady());

        // oC10 meldet keine Spaces: bereit, ohne je zu fragen
        _account->setCapabilities(capabilities(_account->url(), false));
        QVERIFY(_account->spacesManager()->isReady());
        QTest::qWait(100);
        QCOMPARE(_drivesRequests, 0);
    }

    void testDrivesRequestOnceSpacesAreAnnounced()
    {
        createAccount();
        QSignalSpy ready(_account->spacesManager(), &GraphApi::SpacesManager::ready);
        Q_EMIT _account->credentialsFetched();
        QCOMPARE(_drivesRequests, 0);

        _account->setCapabilities(capabilities(_account->url(), true));
        QVERIFY(ready.wait());
        QCOMPARE(_drivesRequests, 1);

        // spätere Capabilities (jede Verbindungsprüfung) fragen nicht erneut
        _account->setCapabilities(capabilities(_account->url(), true));
        QTest::qWait(100);
        QCOMPARE(_drivesRequests, 1);
    }
};

QTEST_GUILESS_MAIN(TestSpacesManager)
#include "testspacesmanager.moc"
