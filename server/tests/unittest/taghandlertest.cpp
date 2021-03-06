/*
    Copyright (c) 2014 Christian Mollekopf <mollekopf@kolabsys.com>

    This library is free software; you can redistribute it and/or modify it
    under the terms of the GNU Library General Public License as published by
    the Free Software Foundation; either version 2 of the License, or (at your
    option) any later version.

    This library is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Library General Public
    License for more details.

    You should have received a copy of the GNU Library General Public License
    along with this library; see the file COPYING.LIB.  If not, write to the
    Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
    02110-1301, USA.
*/

#include <QObject>

#include <imapstreamparser.h>
#include <response.h>
#include <storage/selectquerybuilder.h>

#include "fakeakonadiserver.h"
#include "aktest.h"
#include "akdebug.h"
#include "entities.h"
#include "dbinitializer.h"

#include <QtTest/QTest>

using namespace Akonadi;
using namespace Akonadi::Server;

Q_DECLARE_METATYPE(Akonadi::Server::Tag::List);
Q_DECLARE_METATYPE(Akonadi::Server::Tag);

static NotificationMessageV3::List extractNotifications(QSignalSpy *notificationSpy)
{
    NotificationMessageV3::List receivedNotifications;
    for (int q = 0; q < notificationSpy->size(); q++) {
        //Only one notify call
        if (notificationSpy->at(q).count() != 1) {
            qWarning() << "Error: We're assuming only one notify call.";
            return NotificationMessageV3::List();
        }
        const NotificationMessageV3::List n = notificationSpy->at(q).first().value<NotificationMessageV3::List>();
        for (int i = 0; i < n.size(); i++) {
            // qDebug() << n.at(i);
            receivedNotifications.append(n.at(i));
        }
    }
    return receivedNotifications;
}

class TagHandlerTest : public QObject
{
    Q_OBJECT

public:
    TagHandlerTest()
        : QObject()
    {
        qRegisterMetaType<Akonadi::Server::Response>();
        qRegisterMetaType<Akonadi::Server::Tag::List>();

        try {
            FakeAkonadiServer::instance()->setPopulateDb(false);
            FakeAkonadiServer::instance()->init();
        } catch (const FakeAkonadiServerException &e) {
            akError() << "Server exception: " << e.what();
            akFatal() << "Fake Akonadi Server failed to start up, aborting test";
        }
    }

    ~TagHandlerTest()
    {
        FakeAkonadiServer::instance()->quit();
    }

    QScopedPointer<DbInitializer> initializer;

private Q_SLOTS:
    void testStoreTag_data()
    {
        initializer.reset(new DbInitializer);
        Resource res = initializer->createResource("testresource");

        QTest::addColumn<QList<QByteArray> >("scenario");
        QTest::addColumn<Tag::List>("expectedTags");
        QTest::addColumn<Akonadi::NotificationMessageV3::List>("expectedNotifications");

        {
            QList<QByteArray> scenario;
            scenario << FakeAkonadiServer::defaultScenario()
            << "C: 2 TAGAPPEND (GID \"tag\" MIMETYPE \"PLAIN\" TAG \"(\"tag4\" \"\" \"\" \"\" \"0\" () () \"-1\")\")"
            << "S: * 1 TAGFETCH (UID 1 GID \"tag\" PARENT 0 MIMETYPE \"PLAIN\" TAG \"(\" tag4 \" \"   \" \"   \"0\"  () ()  \"-1\" ) \"\")"
            << "S: 2 OK Append completed";

            Tag tag;
            tag.setId(1);
            TagType type;
            type.setName(QLatin1String("PLAIN"));
            tag.setTagType(type);

            Akonadi::NotificationMessageV3 notification;
            notification.setType(NotificationMessageV2::Tags);
            notification.setOperation(NotificationMessageV2::Add);
            notification.setSessionId(FakeAkonadiServer::instanceName().toLatin1());
            notification.addEntity(1);

            QTest::newRow("uid create relation") << scenario << (Tag::List() << tag) << (NotificationMessageV3::List() << notification);
        }
    }

    void testStoreTag()
    {
        QFETCH(QList<QByteArray>, scenario);
        QFETCH(Tag::List, expectedTags);
        QFETCH(NotificationMessageV3::List, expectedNotifications);

        FakeAkonadiServer::instance()->setScenario(scenario);
        FakeAkonadiServer::instance()->runTest();

        const NotificationMessageV3::List receivedNotifications = extractNotifications(FakeAkonadiServer::instance()->notificationSpy());

        QCOMPARE(receivedNotifications.size(), expectedNotifications.count());
        for (int i = 0; i < expectedNotifications.size(); i++) {
            QCOMPARE(receivedNotifications.at(i), expectedNotifications.at(i));
        }

        const Tag::List tags = Tag::retrieveAll();
        QCOMPARE(tags.size(), expectedTags.size());
        for (int i = 0; i < tags.size(); i++) {
            QCOMPARE(tags.at(i).id(), expectedTags.at(i).id());
            QCOMPARE(tags.at(i).tagType().name(), QString::fromLatin1("PLAIN")/* expectedTags.at(i).tagType().name() */);
        }
    }

    void testModifyTag_data()
    {
        initializer.reset(new DbInitializer);
        Resource res = initializer->createResource("testresource");

        Tag tag;
        TagType type;
        type.setName(QLatin1String("PLAIN"));
        type.insert();
        tag.setTagType(type);
        tag.setGid(QLatin1String("gid"));
        tag.insert();

        QTest::addColumn<QList<QByteArray> >("scenario");
        QTest::addColumn<Tag::List>("expectedTags");
        QTest::addColumn<Akonadi::NotificationMessageV3::List>("expectedNotifications");
        {
            QList<QByteArray> scenario;
            scenario << FakeAkonadiServer::defaultScenario()
            << "C: 2 UID TAGSTORE " + QByteArray::number(tag.id()) + " (MIMETYPE \"PLAIN\" TAG \"(\\\"tag2\\\" \\\"\\\" \\\"\\\" \\\"\\\" \\\"0\\\" () () \\\"-1\\\")\")"
            << "S: * 2 TAGFETCH (UID " + QByteArray::number(tag.id()) + " GID \"gid\" PARENT 0 MIMETYPE \"PLAIN\" TAG \"(\\\"tag2\\\" \\\"\\\" \\\"\\\" \\\"\\\" \\\"0\\\" () () \\\"-1\\\")\")"
            << "S: 2 OK TAGSTORE completed";


            Akonadi::NotificationMessageV3 notification;
            notification.setType(NotificationMessageV2::Tags);
            notification.setOperation(NotificationMessageV2::Modify);
            notification.setSessionId(FakeAkonadiServer::instanceName().toLatin1());
            notification.addEntity(tag.id());

            QTest::newRow("uid store name") << scenario << (Tag::List() << tag) << (NotificationMessageV3::List() << notification);
        }

        {
            QList<QByteArray> scenario;
            scenario << FakeAkonadiServer::defaultScenario()
            << FakeAkonadiServer::selectResourceScenario(QLatin1String("testresource"))
            << "C: 2 UID TAGSTORE " + QByteArray::number(tag.id()) + " (REMOTEID \"remote1\" MIMETYPE \"PLAIN\" TAG \"(\\\"tag1\\\" \\\"\\\" \\\"\\\" \\\"\\\" \\\"0\\\" () () \\\"-1\\\")\")"
            << "S: * 2 TAGFETCH (UID " + QByteArray::number(tag.id()) + " GID \"gid\" PARENT 0 MIMETYPE \"PLAIN\" REMOTEID remote1 TAG \"(\\\"tag1\\\" \\\"\\\" \\\"\\\" \\\"\\\" \\\"0\\\" () () \\\"-1\\\")\")"
            << "S: 2 OK TAGSTORE completed";


            Akonadi::NotificationMessageV3 notification;
            notification.setType(NotificationMessageV2::Tags);
            notification.setOperation(NotificationMessageV2::Modify);
            notification.setSessionId(FakeAkonadiServer::instanceName().toLatin1());
            notification.addEntity(tag.id());

            QTest::newRow("uid store rid") << scenario << (Tag::List() << tag) << (NotificationMessageV3::List() << notification);
        }
    }

    void testModifyTag()
    {
        QFETCH(QList<QByteArray>, scenario);
        QFETCH(Tag::List, expectedTags);
        QFETCH(NotificationMessageV3::List, expectedNotifications);

        FakeAkonadiServer::instance()->setScenario(scenario);
        FakeAkonadiServer::instance()->runTest();

        const NotificationMessageV3::List receivedNotifications = extractNotifications(FakeAkonadiServer::instance()->notificationSpy());

        QCOMPARE(receivedNotifications.size(), expectedNotifications.count());
        for (int i = 0; i < receivedNotifications.size(); i++) {
            QCOMPARE(receivedNotifications.at(i), expectedNotifications.at(i));
        }

        const Tag::List tags = Tag::retrieveAll();
        QCOMPARE(tags.size(), expectedTags.size());
        for (int i = 0; i < tags.size(); i++) {
            QCOMPARE(tags.at(i).id(), expectedTags.at(i).id());
            QCOMPARE(tags.at(i).tagType().name(), expectedTags.at(i).tagType().name());
        }
    }
};

AKTEST_FAKESERVER_MAIN(TagHandlerTest)

#include "taghandlertest.moc"
