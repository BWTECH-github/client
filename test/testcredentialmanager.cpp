/*
 *    This software is in the public domain, furnished "as is", without technical
 *    support, and with no warranty, express or implied, as to its usefulness for
 *    any purpose.
 *
 */
#include "account.h"
#include "accountstate.h"
#include "configfile.h"
#include "libsync/creds/credentialmanager.h"
#include "theme.h"

#include "testutils/syncenginetestutils.h"

#include <QTest>

namespace OCC {

class TestCredentialManager : public QObject
{
    Q_OBJECT

    bool _finished = false;

    QTemporaryFile _credStoreFile;
    QSettings _credStore;

    void setFallbackEnabled(QKeychain::Job *job)
    {
        // store the test credentials in a plain text settings file on unsupported platforms
        job->setSettings(&_credStore);
        job->setInsecureFallback(true);
    }

private Q_SLOTS:
    void init()
    {
        _finished = false;
        QVERIFY(_credStoreFile.open());
        _credStore.setPath(QSettings::IniFormat, QSettings::UserScope, _credStoreFile.fileName());
        _credStore.clear();
    }

    void testSetGet_data()
    {
        QTest::addColumn<QVariant>("data");

        QTest::newRow("bool") << QVariant::fromValue(true);
        QTest::newRow("int") << QVariant::fromValue(1);
        QTest::newRow("map") << QVariant::fromValue(
            QVariantMap{{QStringLiteral("foo"), QColor(Qt::red).name()}, {QStringLiteral("bar"), QStringLiteral("42")}});
    }

    void testSetGet()
    {
        QFETCH(QVariant, data);
        FakeFolder fakeFolder { FileInfo::A12_B12_C12_S12() };
        auto creds = fakeFolder.account()->credentialManager();

        const QString key = QStringLiteral("test");
        auto setJob = creds->set(key, data);
        setFallbackEnabled(setJob);

        connect(setJob, &QKeychain::Job::finished, this, [creds, data, key, setJob, this] {
#ifdef Q_OS_LINUX
            if (!qEnvironmentVariableIsSet("DBUS_SESSION_BUS_ADDRESS")) {
                QEXPECT_FAIL("", "QKeychain might not use the plaintext fallback and fail if dbus is not present", Abort);
                QCOMPARE(setJob->error(), QKeychain::NoError);
            }
#endif
            QCOMPARE(setJob->error(), QKeychain::NoError);
            auto getJob = creds->get(key);
            QVERIFY(getJob != nullptr);
            setFallbackEnabled(getJob->_job);
            connect(getJob, &CredentialJob::finished, this, [getJob, data, creds, this] {
                QCOMPARE(getJob->error(), QKeychain::NoError);
                QCOMPARE(getJob->data(), data);
                const auto jobs = creds->clear();
                for (auto &job : jobs) {
                    setFallbackEnabled(job);
                }
                connect(jobs[0], &QKeychain::Job::finished, this, [creds, data, jobs, this] {
                    QCOMPARE(jobs[0]->error(), QKeychain::NoError);
                    QVERIFY(creds->isEmpty());
                    _finished = true;
                });
            });
        });
#ifdef Q_OS_LINUX
        // As we have the skip condition on linux the wait might time out here.
        bool ok = QTest::qWaitFor([this] { return _finished; });
        Q_UNUSED(ok)
#else
        QVERIFY(QTest::qWaitFor([this] { return _finished; }));
#endif
    }

    // nach einem Serverumzug (neuer Host) liest der Manager die bekannten Schlüssel aus dem neuen Bereich, nicht mehr aus dem
    // alten: sonst landete der neue Token unter dem alten Bereich und wäre nach einem Neustart verloren
    void testScopeFollowsServerUrl()
    {
        auto accountState = createDummyAccount();
        Account *account = accountState->account();
        CredentialManager *creds = account->credentialManager();
        const QString key = QStringLiteral("scope-test");
        const QString oldHost = account->url().host();
        const auto scopeGroup = [account](const QString &host) {
            return QStringLiteral("%1_credentials:%2:%3").arg(Theme::instance()->appName(), host, account->uuid().toString(QUuid::WithoutBraces));
        };

        {
            auto oldScope = ConfigFile::settingsWithGroup(QStringLiteral("Credentials/") + scopeGroup(oldHost));
            oldScope->setValue(key, true);
            oldScope->sync();
        }
        QVERIFY(creds->contains(key));

        account->setUrl(QUrl(QStringLiteral("https://neu.example.org/owncloud")));
        QVERIFY(!creds->contains(key));

        auto credentials = ConfigFile::settingsWithGroup(QStringLiteral("Credentials"));
        credentials->remove(scopeGroup(oldHost));
        credentials->remove(scopeGroup(QStringLiteral("neu.example.org")));
    }

    void testSetGet2()
    {
        FakeFolder fakeFolder { FileInfo::A12_B12_C12_S12() };
        auto creds = fakeFolder.account()->credentialManager();

        const QVariantMap data {
            { QStringLiteral("foo/test"), QColor(Qt::red) },
            { QStringLiteral("foo/test2"), QColor(Qt::gray) },
            { QStringLiteral("bar/test"), QColor(Qt::blue) },
            { QStringLiteral("narf/test"), QColor(Qt::green) }
        };

        QVector<QSignalSpy *> spies;
        for (auto it = data.cbegin(); it != data.cend(); ++it) {
            auto setJob = creds->set(it.key(), it.value());
            setFallbackEnabled(setJob);
            spies.append(new QSignalSpy(setJob, &QKeychain::Job::finished));
        }
        QTest::qWait(1000);
        for (const auto s : spies) {
            QCOMPARE(s->count(), 1);
        }
        qDeleteAll(spies);
        spies.clear();
        {
            auto jobs = creds->clear(QStringLiteral("foo"));
#ifdef Q_OS_LINUX
            if (!qEnvironmentVariableIsSet("DBUS_SESSION_BUS_ADDRESS")) {
                QEXPECT_FAIL("", "QKeychain might not use the plaintext fallback and fail if dbus is not present", Abort);
                QCOMPARE(jobs.size(), 2);
            }
#endif
            QCOMPARE(jobs.size(), 2);
            for (auto &job : jobs) {
                setFallbackEnabled(job);
                spies.append(new QSignalSpy(job, &QKeychain::Job::finished));
            }
            QTest::qWait(1000);
            for (const auto s : spies) {
                QCOMPARE(s->count(), 1);
            }
            qDeleteAll(spies);
        }
    }
};
}

QTEST_GUILESS_MAIN(OCC::TestCredentialManager)
#include "testcredentialmanager.moc"
