/*
    Copyright (c) 2014 Daniel Vrátil <dvratil@redhat.com>

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
#include <QSettings>

#include <handler/akappend.h>
#include <imapstreamparser.h>
#include <response.h>
#include <storage/selectquerybuilder.h>


#include <libs/notificationmessagev3_p.h>
#include <libs/notificationmessagev2_p.h>
#include <imapparser_p.h>

#include "fakeakonadiserver.h"
#include "fakeentities.h"
#include "aktest.h"
#include <akstandarddirs.h>

#include <QtTest/QTest>
#include <QSignalSpy>

using namespace Akonadi;
using namespace Akonadi::Server;

Q_DECLARE_METATYPE(PimItem)
Q_DECLARE_METATYPE(QVector<Flag>)
Q_DECLARE_METATYPE(QVector<FakePart>)
Q_DECLARE_METATYPE(QVector<FakeTag>)

class AkAppendHandlerTest : public QObject
{
    Q_OBJECT

public:
    AkAppendHandlerTest()
    {
        qRegisterMetaType<Akonadi::Server::Response>();

        // Effectively disable external payload parts, we have a dedicated unit-test
        // for that
        const QString serverConfigFile = AkStandardDirs::serverConfigFile(XdgBaseDirs::ReadWrite);
        QSettings settings(serverConfigFile, QSettings::IniFormat);
        settings.setValue(QLatin1String("General/SizeThreshold"), std::numeric_limits<qint64>::max());

        try {
            FakeAkonadiServer::instance()->init();
        } catch (FakeAkonadiServerException &e) {
            akError() << e.what();
            akFatal() << "Fake Akonadi Server failed to start up, aborting test";
        }
    }

    ~AkAppendHandlerTest()
    {
        FakeAkonadiServer::instance()->quit();
    }

    void updatePimItem(PimItem &pimItem, const QString &remoteId, const qint64 size)
    {
        pimItem.setRemoteId(remoteId);
        pimItem.setGid(remoteId);
        pimItem.setSize(size);
    }

    void updateNotifcationEntity(NotificationMessageV3 &ntf, const PimItem &pimItem)
    {
        ntf.clearEntities();
        ntf.addEntity(pimItem.id(), pimItem.remoteId(), pimItem.remoteRevision(), pimItem.mimeType().name());
    }

    struct PartHelper {
        PartHelper(const QString &type_, const QByteArray &data_, int size_, bool external_ = false, int version_ = 0)
            : type(type_), data(data_), size(size_), external(external_), version(version_)
        {}
        QString type;
        QByteArray data;
        int size;
        bool external;
        int version;
    };

    void updateParts(QVector<FakePart> &parts, const std::vector<PartHelper> &updatedParts)
    {
        parts.clear();
        Q_FOREACH (const PartHelper &helper, updatedParts) {
            FakePart part;

            const QStringList types = helper.type.split(QLatin1Char(':'));
            Q_ASSERT(types.count() == 2);
            part.setPartType(PartType(types[1], types[0]));
            part.setData(helper.data);
            part.setDatasize(helper.size);
            part.setExternal(helper.external);
            part.setVersion(helper.version);
            parts << part;
        }
    }

    void updateFlags(QVector<Flag> &flags, const QStringList &updatedFlags)
    {
        flags.clear();
        Q_FOREACH (const QString &flagName, updatedFlags) {
            Flag flag;
            flag.setName(flagName);
            flags << flag;
        }
    }

    struct TagHelper {
        TagHelper(const QString &tagType_, const QString &gid_, const QString &remoteId_ = QString())
            : tagType(tagType_), gid(gid_), remoteId(remoteId_)
        {}
        QString tagType;
        QString gid;
        QString remoteId;
    };
    void updateTags(QVector<FakeTag> &tags, const std::vector<TagHelper> &updatedTags)
    {
        tags.clear();
        Q_FOREACH (const TagHelper &helper, updatedTags) {
            FakeTag tag;

            TagType tagType;
            tagType.setName(helper.tagType);

            tag.setTagType(tagType);
            tag.setGid(helper.gid);
            tag.setRemoteId(helper.remoteId);
            tags << tag;
        }
    }

    QByteArray createCommand(const PimItem &pimItem, const QDateTime &dt,
                             qint64 overrideSize = -1)
    {
        const qint64 size = overrideSize > -1 ? overrideSize : pimItem.size();
        return "C: 2 X-AKAPPEND " + QByteArray::number(pimItem.collectionId()) + " "
                                  + QByteArray::number(size) + " "
                                  + "(\\RemoteId[" + pimItem.remoteId().toLatin1() + "] "
                                  +  "\\MimeType[" + pimItem.mimeType().name().toLatin1() + "] "
                                  +  "\\RemoteRevision[" + pimItem.remoteRevision().toLatin1() + "] "
                                  +  "\\Gid[" + pimItem.gid().toLatin1() + "]) "
                                  + "\"" + dt.toString(QLatin1String("dd-MMM-yyyy hh:mm:ss")).toLatin1() + " +0000\"";
    }

private Q_SLOTS:
    void testAkAppend_data()
    {
        QTest::addColumn<QList<QByteArray> >("scenario");
        QTest::addColumn<NotificationMessageV3>("notification");
        QTest::addColumn<PimItem>("pimItem");
        QTest::addColumn<QVector<FakePart> >("parts");
        QTest::addColumn<QVector<Flag> >("flags");
        QTest::addColumn<QVector<FakeTag> >("tags");
        QTest::addColumn<qint64>("uidnext");
        QTest::addColumn<QDateTime>("datetime");
        QTest::addColumn<bool>("expectFail");

        QList<QByteArray> scenario;
        NotificationMessageV3 notification;
        qint64 uidnext = 0;
        QDateTime datetime(QDate(2014, 05, 12), QTime(14, 46, 00), Qt::UTC);
        PimItem pimItem;
        QVector<FakePart> parts;
        QVector<Flag> flags;
        QVector<FakeTag> tags;

        pimItem.setCollectionId(4);
        pimItem.setSize(10);
        pimItem.setRemoteId(QLatin1String("TEST-1"));
        pimItem.setRemoteRevision(QLatin1String("1"));
        pimItem.setGid(QLatin1String("TEST-1"));
        pimItem.setMimeType(MimeType::retrieveByName(QLatin1String("application/octet-stream")));
        pimItem.setDatetime(datetime);
        updateParts(parts, { { QLatin1String("PLD:DATA"), "0123456789", 10 } });
        notification.setType(NotificationMessageV2::Items);
        notification.setOperation(NotificationMessageV2::Add);
        notification.setParentCollection(4);
        notification.setResource("akonadi_fake_resource_0");
        notification.addEntity(-1, QLatin1String("TEST-1"), QLatin1String("1"), QLatin1String("application/octet-stream"));
        notification.setSessionId(FakeAkonadiServer::instanceName().toLatin1());
        uidnext = 13;
        scenario << FakeAkonadiServer::defaultScenario()
                 << createCommand(pimItem, datetime) + " (PLD:DATA[0] {10}"
                 << "S: + Ready for literal data (expecting 10 bytes)"
                 << "C: 0123456789)"
                 << "S: 2 [UIDNEXT 13 DATETIME \"12-May-2014 14:46:00 +0000\"]"
                 << "S: 2 OK Append completed";
        QTest::newRow("single-part") << scenario << notification << pimItem << parts
                                     << flags << tags << uidnext << datetime << false;


        updatePimItem(pimItem, QLatin1String("TEST-2"), 20);
        updateParts(parts, { { QLatin1String("PLD:DATA"), "Random Data", 11 },
                             { QLatin1String("PLD:PLDTEST"), "Test Data", 9 } });
        updateNotifcationEntity(notification, pimItem);
        ++uidnext;
        scenario.clear();
        scenario << FakeAkonadiServer::defaultScenario()
                 << createCommand(pimItem, datetime) + " (PLD:DATA[0] {11}"
                 << "S: + Ready for literal data (expecting 11 bytes)"
                 << "C: Random Data PLD:PLDTEST {9}"
                 << "S: + Ready for literal data (expecting 9 bytes)"
                 << "C: Test Data)"
                 << "S: 2 [UIDNEXT " + QByteArray::number(uidnext) + " DATETIME \"12-May-2014 14:46:00 +0000\"]"
                 << "S: 2 OK Append completed";
        QTest::newRow("multi-part") << scenario << notification << pimItem << parts
                                    << flags << tags << uidnext << datetime << false;


        scenario.clear();
        scenario << FakeAkonadiServer::defaultScenario()
                 << "C: 2 X-AKAPPEND 100 0 () ()"
                 << "S: 2 NO Unknown collection for '100'.";
        QTest::newRow("invalid collection") << scenario << NotificationMessageV3()
                                            << PimItem() << QVector<FakePart>()
                                            << QVector<Flag>() << QVector<FakeTag>()
                                            << -1ll << QDateTime() << true;


        scenario.clear();
        scenario << FakeAkonadiServer::defaultScenario()
                 << "C: 2 X-AKAPPEND 6 0 () ()"
                 << "S: 2 NO Cannot append item into virtual collection";
        QTest::newRow("virtual collection") << scenario << NotificationMessageV3()
                                            << PimItem() << QVector<FakePart>()
                                            << QVector<Flag>() << QVector<FakeTag>()
                                            << -1ll << QDateTime() << true;


        updatePimItem(pimItem, QLatin1String("TEST-3"), 5);
        updateParts(parts, { { QLatin1String("PLD:DATA"), "12345", 5 } });
        updateNotifcationEntity(notification, pimItem);
        ++uidnext;
        scenario.clear();
        scenario << FakeAkonadiServer::defaultScenario()
                 << createCommand(pimItem, datetime, 1) + " (PLD:DATA[0] {5}"
                 << "S: + Ready for literal data (expecting 5 bytes)"
                 << "C: 12345)"
                 << "S: 2 [UIDNEXT " + QByteArray::number(uidnext) + " DATETIME \"12-May-2014 14:46:00 +0000\"]"
                 << "S: 2 OK Append completed";
        QTest::newRow("mismatch item sizes (smaller)") << scenario << notification << pimItem
                                                       << parts << flags << tags << uidnext
                                                       << datetime << false;


        updatePimItem(pimItem, QLatin1String("TEST-4"), 1000);
        updateNotifcationEntity(notification, pimItem);
        ++uidnext;
        scenario.clear();
        scenario << FakeAkonadiServer::defaultScenario()
                 << createCommand(pimItem, datetime) + " (PLD:DATA[0] {5}"
                 << "S: + Ready for literal data (expecting 5 bytes)"
                 << "C: 12345)"
                 << "S: 2 [UIDNEXT " + QByteArray::number(uidnext) + " DATETIME \"12-May-2014 14:46:00 +0000\"]"
                 << "S: 2 OK Append completed";
        QTest::newRow("mismatch item sizes (bigger)") << scenario << notification << pimItem
                                                      << parts << flags << tags << uidnext
                                                      << datetime << false;


        scenario.clear();
        scenario << FakeAkonadiServer::defaultScenario()
                 << createCommand(pimItem, datetime) + " (PLD:DATA[0] {4}"
                 << "S: + Ready for literal data (expecting 4 bytes)"
                 << "C: 123"
                 << "S: 2 NO ImapParserException: Unable to read more data";
        QTest::newRow("incomplete part data") << scenario << NotificationMessageV3()
                                              << PimItem() << QVector<FakePart>()
                                              << QVector<Flag>() << QVector<FakeTag>()
                                              << -1ll << QDateTime() << true;


        scenario.clear();
        scenario << FakeAkonadiServer::defaultScenario()
                 << createCommand(pimItem, datetime) + " (PLD:DATA[0] {4}"
                 << "S: + Ready for literal data (expecting 4 bytes)"
                 << "C: 12345678910"
                 << "S: 2 NO PartTypeException: Invalid part type name.";
        QTest::newRow("part data larger than advertised") << scenario << NotificationMessageV3()
                                                          << PimItem() << QVector<FakePart>()
                                                          << QVector<Flag>() << QVector<FakeTag>()
                                                          << -1ll << QDateTime() << true;


        updatePimItem(pimItem, QLatin1String("TEST-5"), 0);
        updateParts(parts, { { QLatin1String("PLD:DATA"), QByteArray(), 0 } });
        updateNotifcationEntity(notification, pimItem);
        ++uidnext;
        scenario.clear();
        scenario << FakeAkonadiServer::defaultScenario()
                 << createCommand(pimItem, datetime) + " (PLD:DATA[0] NIL)"
                 << "S: 2 [UIDNEXT " + QByteArray::number(uidnext) + " DATETIME \"12-May-2014 14:46:00 +0000\"]"
                 << "S: 2 OK Append completed";
        QTest::newRow("empty payload part") << scenario << notification << pimItem << parts
                                            << flags << tags << uidnext << datetime << false;


        updatePimItem(pimItem, QLatin1String("TEST-7"), 0);
        updateNotifcationEntity(notification, pimItem);
        ++uidnext;
        scenario.clear();
        scenario << FakeAkonadiServer::defaultScenario()
                << createCommand(pimItem, datetime) + " (PLD:DATA[0] {0}"
                 << "S: + Ready for literal data (expecting 0 bytes)"
                 << "C: )"
                 << "S: 2 [UIDNEXT " + QByteArray::number(uidnext) + " DATETIME \"12-May-2014 14:46:00 +0000\"]"
                 << "S: 2 OK Append completed";
        QTest::newRow("empty part data") << scenario << notification << pimItem << parts
                                         << flags << tags << uidnext << datetime << false;


        updatePimItem(pimItem, QLatin1String("TEST-8"), 1);
        updateParts(parts, { { QLatin1String("PLD:DATA"), QByteArray("\0", 1), 1 } });
        updateNotifcationEntity(notification, pimItem);
        ++uidnext;
        scenario.clear();
        scenario << FakeAkonadiServer::defaultScenario()
                 << createCommand(pimItem,  datetime) + " (PLD:DATA[0] {1}"
                 << "S: + Ready for literal data (expecting 1 bytes)"
                 << "C: " + QByteArray("\0", 1) + ")"   // otherwise QByteArray trims the string before"\0"
                 << "S: 2 [UIDNEXT " + QByteArray::number(uidnext) + " DATETIME \"12-May-2014 14:46:00 +0000\"]"
                 << "S: 2 OK Append completed";
        QTest::newRow("part data will null character") << scenario << notification << pimItem
                                                       << parts << flags << tags << uidnext
                                                       << datetime << false;


        const QString utf8String = QString::fromUtf8("äöüß@€µøđ¢©®");
        updatePimItem(pimItem, QLatin1String("TEST-9"), utf8String.toUtf8().size());
        updateParts(parts, { { QLatin1String("PLD:DATA"), utf8String.toUtf8(), utf8String.toUtf8().size() } });
        updateNotifcationEntity(notification, pimItem);
        ++uidnext;
        scenario.clear();
        scenario << FakeAkonadiServer::defaultScenario()
                 << createCommand(pimItem, datetime) + " (PLD:DATA[0] {" + QByteArray::number(parts.first().datasize()) + "}"
                 << "S: + Ready for literal data (expecting " + QByteArray::number(parts.first().datasize()) + " bytes)"
                 << "C: " + utf8String.toUtf8() + ")"
                 << "S: 2 [UIDNEXT " + QByteArray::number(uidnext) + " DATETIME \"12-May-2014 14:46:00 +0000\"]"
                 << "S: 2 OK Append completed";
        QTest::newRow("utf8 part data") << scenario << notification << pimItem << parts
                                        << flags << tags << uidnext << datetime << false;


        const QByteArray hugeData = QByteArray("a").repeated(1 << 20);
        updatePimItem(pimItem, QLatin1String("TEST-10"), 1 << 20);
        updateParts(parts, { { QLatin1String("PLD:DATA"), hugeData, 1 << 20 } });
        updateNotifcationEntity(notification, pimItem);
        ++uidnext;
        scenario.clear();
        scenario << FakeAkonadiServer::defaultScenario()
                 << createCommand(pimItem, datetime) + " (PLD:DATA[0] {" + QByteArray::number(parts.first().datasize()) + "}"
                 << "S: + Ready for literal data (expecting " + QByteArray::number(parts.first().datasize()) + " bytes)"
                 << "C: " + hugeData + ")"
                 << "S: 2 [UIDNEXT " + QByteArray::number(uidnext) + " DATETIME \"12-May-2014 14:46:00 +0000\"]"
                 << "S: 2 OK Append completed";
        QTest::newRow("huge part data") << scenario << notification << pimItem << parts
                                        << flags << tags << uidnext << datetime << false;


        const QByteArray dataWithNewLines = "Bernard, Bernard, Bernard, Bernard, look, look Bernard!\nWHAT!!!!!!!\nI'm a prostitute robot from the future!";
        updatePimItem(pimItem, QLatin1String("TEST-11"), dataWithNewLines.size());
        updateParts(parts, { { QLatin1String("PLD:DATA"), dataWithNewLines, dataWithNewLines.size() } });
        updateNotifcationEntity(notification, pimItem);
        ++uidnext;
        scenario.clear();
        scenario << FakeAkonadiServer::defaultScenario()
                 << createCommand(pimItem, datetime) + " (PLD:DATA[0] {" + QByteArray::number(parts.first().datasize()) + "}"
                 << "S: + Ready for literal data (expecting " + QByteArray::number(parts.first().datasize()) + " bytes)"
                 << "C: " + dataWithNewLines + ")"
                 << "S: 2 [UIDNEXT " + QByteArray::number(uidnext) + " DATETIME \"12-May-2014 14:46:00 +0000\"]"
                 << "S: 2 OK Append completed";
        QTest::newRow("data with newlines") << scenario << notification << pimItem << parts
                                            << flags << tags << uidnext << datetime << false;


        const QByteArray lotsOfNewlines = QByteArray("\n").repeated(1 << 20);
        updatePimItem(pimItem, QLatin1String("TEST-12"), lotsOfNewlines.size());
        updateParts(parts, { { QLatin1String("PLD:DATA"), lotsOfNewlines, lotsOfNewlines.size() } });
        updateNotifcationEntity(notification, pimItem);
        ++uidnext;
        scenario.clear();
        scenario << FakeAkonadiServer::defaultScenario()
                 << createCommand(pimItem, datetime) + "(PLD:DATA[0] {" + QByteArray::number(parts.first().datasize()) + "}"
                 << "S: + Ready for literal data (expecting " + QByteArray::number(parts.first().datasize()) + " bytes)"
                 << "C: " + lotsOfNewlines + ")"
                 << "S: 2 [UIDNEXT " + QByteArray::number(uidnext) + " DATETIME \"12-May-2014 14:46:00 +0000\"]"
                 << "S: 2 OK Append completed";
        QTest::newRow("data with lots of newlines") << scenario << notification << pimItem
                                                    << parts << flags << tags << uidnext
                                                    << datetime << false;


        updatePimItem(pimItem, QLatin1String("TEST-13"), 20);
        updateParts(parts, { { QLatin1String("PLD:NEWPARTTYPE1"), "0123456789", 10 },
                             { QLatin1String("PLD:NEWPARTTYPE2"), "9876543210", 10 } });
        updateNotifcationEntity(notification, pimItem);
        ++uidnext;
        scenario.clear();
        scenario << FakeAkonadiServer::defaultScenario()
                 << createCommand(pimItem, datetime) + "(PLD:NEWPARTTYPE1[0] {10}"
                 << "S: + Ready for literal data (expecting 10 bytes)"
                 << "C: 0123456789 PLD:NEWPARTTYPE2[0] {10}"
                 << "S: + Ready for literal data (expecting 10 bytes)"
                 << "C: 9876543210)"
                 << "S: 2 [UIDNEXT " + QByteArray::number(uidnext) + " DATETIME \"12-May-2014 14:46:00 +0000\"]"
                 << "S: 2 OK Append completed";
        QTest::newRow("non-existent part types") << scenario << notification << pimItem
                                                 << parts << flags << tags << uidnext
                                                 << datetime << false;


        updatePimItem(pimItem, QLatin1String("TEST-14"), 0);
        updateParts(parts, {});
        updateFlags(flags, QStringList() << QLatin1String("\\SEEN") << QLatin1String("\\RANDOM") );
        updateNotifcationEntity(notification, pimItem);
        ++uidnext;
        scenario.clear();
        scenario << FakeAkonadiServer::defaultScenario()
                 << "C: 2 X-AKAPPEND 4 0 (\\RemoteId[TEST-14] \\MimeType[application/octet-stream] \\RemoteRevision[1] \\Gid[TEST-14] \\SEEN \\RANDOM) \"12-May-2014 14:46:00 +0000\" ()"
                 << "S: 2 [UIDNEXT " + QByteArray::number(uidnext) + " DATETIME \"12-May-2014 14:46:00 +0000\"]"
                 << "S: 2 OK Append completed";
        QTest::newRow("item with flags") << scenario << notification << pimItem << parts
                                         << flags << tags << uidnext << datetime << false;


        updatePimItem(pimItem, QLatin1String("TEST-15"), 0);
        updateFlags(flags, {});
        updateTags(tags, { { QLatin1String("PLAIN"), QLatin1String("TAG-1") },
                           { QLatin1String("PLAIN"), QLatin1String("TAG-2") } });
        updateNotifcationEntity(notification, pimItem);
        ++uidnext;
        scenario.clear();
        scenario << FakeAkonadiServer::defaultScenario()
                 << "C: 2 X-AKAPPEND 4 0 (\\RemoteId[TEST-15] \\MimeType[application/octet-stream] \\RemoteRevision[1] \\Gid[TEST-15] \\Tag[TAG-1] \\Tag[TAG-2]) \"12-May-2014 14:46:00 +0000\" ()"
                 << "S: 2 [UIDNEXT " + QByteArray::number(uidnext) + " DATETIME \"12-May-2014 14:46:00 +0000\"]"
                 << "S: 2 OK Append completed";
        QTest::newRow("item with non-existent tags (GID)") << scenario << notification << pimItem << parts
                                                           << flags << tags << uidnext << datetime << false;


        updatePimItem(pimItem, QLatin1String("TEST-16"), 0);
        updateTags(tags, { { QLatin1String("PLAIN"), QLatin1String("TAG-3") },
                           { QLatin1String("PLAIN"), QLatin1String("TAG-4") } });
        updateNotifcationEntity(notification, pimItem);
        ++uidnext;
        scenario.clear();
        scenario << FakeAkonadiServer::defaultScenario()
                 << FakeAkonadiServer::selectResourceScenario(QLatin1String("akonadi_fake_resource_0"))
                 << "C: 3 X-AKAPPEND 4 0 (\\RemoteId[TEST-16] \\MimeType[application/octet-stream] \\RemoteRevision[1] \\Gid[TEST-16] \\RTag[TAG-3] \\RTag[TAG-4]) \"12-May-2014 14:46:00 +0000\" ()"
                 << "S: 3 [UIDNEXT " + QByteArray::number(uidnext) + " DATETIME \"12-May-2014 14:46:00 +0000\"]"
                 << "S: 3 OK Append completed";
        QTest::newRow("item with non-existent tags (RID)") << scenario << notification << pimItem << parts
                                                          << flags << tags << uidnext << datetime << false;


        updatePimItem(pimItem, QLatin1String("TEST-17"), 0);
        updateNotifcationEntity(notification, pimItem);
        updateTags(tags, { { QLatin1String("PLAIN"), QLatin1String("TAG-1") },
                           { QLatin1String("PLAIN"), QLatin1String("TAG-2") } });
        ++uidnext;
        scenario.clear();
        scenario << FakeAkonadiServer::defaultScenario()
                 << FakeAkonadiServer::selectResourceScenario(QLatin1String("akonadi_fake_resource_0"))
                 << "C: 3 X-AKAPPEND 4 0 (\\RemoteId[TEST-17] \\MimeType[application/octet-stream] \\RemoteRevision[1] \\Gid[TEST-17] \\RTag[TAG-1] \\RTag[TAG-2]) \"12-May-2014 14:46:00 +0000\" ()"
                 << "S: 3 [UIDNEXT " + QByteArray::number(uidnext) + " DATETIME \"12-May-2014 14:46:00 +0000\"]"
                 << "S: 3 OK Append completed";
        QTest::newRow("item with existing tags (RID)") << scenario << notification << pimItem << parts
                                                       << flags << tags << uidnext << datetime << false;



        updatePimItem(pimItem, QLatin1String("TEST-18"), 0);
        updateNotifcationEntity(notification, pimItem);
        updateTags(tags, { { QLatin1String("PLAIN"), QLatin1String("TAG-3") },
                           { QLatin1String("PLAIN"), QLatin1String("TAG-4") } });
        ++uidnext;
        scenario.clear();
        scenario << FakeAkonadiServer::defaultScenario()
                 << "C: 2 X-AKAPPEND 4 0 (\\RemoteId[TEST-18] \\MimeType[application/octet-stream] \\RemoteRevision[1] \\Gid[TEST-18] \\Tag[TAG-3] \\Tag[TAG-4]) \"12-May-2014 14:46:00 +0000\" ()"
                 << "S: 2 [UIDNEXT " + QByteArray::number(uidnext) + " DATETIME \"12-May-2014 14:46:00 +0000\"]"
                 << "S: 2 OK Append completed";
        QTest::newRow("item with existing tags (GID)") << scenario << notification << pimItem << parts
                                                       << flags << tags << uidnext << datetime << false;


        updatePimItem(pimItem, QLatin1String("TEST-19"), 0);
        updateFlags(flags, QStringList() << QLatin1String("\\SEEN") << QLatin1String("$FLAG") );
        updateTags(tags, { { QLatin1String("PLAIN"), QLatin1String("TAG-1") },
                           { QLatin1String("PLAIN"), QLatin1String("TAG-2") } });
        updateNotifcationEntity(notification, pimItem);
        ++uidnext;
        scenario.clear();
        scenario << FakeAkonadiServer::defaultScenario()
                 << "C: 2 X-AKAPPEND 4 0 (\\RemoteId[TEST-19] \\MimeType[application/octet-stream] \\RemoteRevision[1] \\Gid[TEST-19] \\Tag[TAG-1] \\SEEN \\Tag[TAG-2] $FLAG) \"12-May-2014 14:46:00 +0000\" ()"
                 << "S: 2 [UIDNEXT " + QByteArray::number(uidnext) + " DATETIME \"12-May-2014 14:46:00 +0000\"]"
                 << "S: 2 OK Append completed";
        QTest::newRow("item with flags and tags") << scenario << notification << pimItem << parts
                                                  << flags << tags << uidnext << datetime << false;


        updatePimItem(pimItem, QLatin1String("TEST-20"), 0);
        updateFlags(flags, {});
        updateTags(tags, { { QLatin1String("PLAIN"), utf8String } });
        updateNotifcationEntity(notification, pimItem);;
        ++uidnext;
        scenario.clear();
        scenario << FakeAkonadiServer::defaultScenario()
                 << "C: 2 X-AKAPPEND 4 0 (\\RemoteId[TEST-20] \\MimeType[application/octet-stream] \\RemoteRevision[1] \\Gid[TEST-20] \\Tag[äöüß@€µøđ¢©®]) \"12-May-2014 14:46:00 +0000\" ()"
                 << "S: 2 [UIDNEXT " + QByteArray::number(uidnext) + " DATETIME \"12-May-2014 14:46:00 +0000\"]"
                 << "S: 2 OK Append completed";
        QTest::newRow("item with UTF-8 tag") << scenario << notification << pimItem << parts
                                             << flags << tags << uidnext << datetime << false;
    }

    void testAkAppend()
    {
        QFETCH(QList<QByteArray>, scenario);
        QFETCH(NotificationMessageV3, notification);
        QFETCH(PimItem, pimItem);
        QFETCH(QVector<FakePart>, parts);
        QFETCH(QVector<Flag>, flags);
        QFETCH(QVector<FakeTag>, tags);
        QFETCH(qint64, uidnext);
        QFETCH(bool, expectFail);

        FakeAkonadiServer::instance()->setScenario(scenario);
        FakeAkonadiServer::instance()->runTest();

        QSignalSpy *notificationSpy = FakeAkonadiServer::instance()->notificationSpy();

        if (notification.isValid()) {
            QCOMPARE(notificationSpy->count(), 1);
            const NotificationMessageV3::List notifications = notificationSpy->at(0).first().value<NotificationMessageV3::List>();
            QCOMPARE(notifications.count(), 1);
            const NotificationMessageV3 itemNotification = notifications.first();

            QVERIFY(AkTest::compareNotifications(itemNotification, notification, QFlag(AkTest::NtfAll &~ AkTest::NtfEntities)));
            QCOMPARE(itemNotification.entities().count(), notification.entities().count());
        } else {
            QVERIFY(notificationSpy->isEmpty());
        }

        const PimItem actualItem = PimItem::retrieveById(uidnext);
        if (expectFail) {
            QVERIFY(!actualItem.isValid());
        } else {
            QVERIFY(actualItem.isValid());
            QCOMPARE(actualItem.remoteId(), pimItem.remoteId());
            QCOMPARE(actualItem.remoteRevision(), pimItem.remoteRevision());
            QCOMPARE(actualItem.gid(), pimItem.gid());
            QCOMPARE(actualItem.size(), pimItem.size());
            QCOMPARE(actualItem.datetime(), pimItem.datetime());
            QCOMPARE(actualItem.collectionId(), pimItem.collectionId());
            QCOMPARE(actualItem.mimeTypeId(), pimItem.mimeTypeId());

            const QList<Flag> actualFlags = actualItem.flags().toList();
            QCOMPARE(actualFlags.count(), flags.count());
            Q_FOREACH (const Flag &flag, flags) {
                const QList<Flag>::const_iterator actualFlagIter =
                    std::find_if(actualFlags.constBegin(), actualFlags.constEnd(),
                                 [flag] (Flag const &actualFlag) {
                                     return flag.name() == actualFlag.name();
                                 });
                QVERIFY(actualFlagIter != actualFlags.constEnd());
                const Flag actualFlag = *actualFlagIter;
                QVERIFY(actualFlag.isValid());
            }

            const QList<Tag> actualTags = actualItem.tags().toList();
            QCOMPARE(actualTags.count(), tags.count());
            Q_FOREACH (const FakeTag &tag, tags) {
                const QList<Tag>::const_iterator actualTagIter =
                    std::find_if(actualTags.constBegin(), actualTags.constEnd(),
                                 [tag] (Tag const &actualTag) {
                                     return tag.gid() == actualTag.gid();
                                 });

                QVERIFY(actualTagIter != actualTags.constEnd());
                const Tag actualTag = *actualTagIter;
                QVERIFY(actualTag.isValid());
                QCOMPARE(actualTag.tagType().name(), tag.tagType().name());
                QCOMPARE(actualTag.gid(), tag.gid());
                if (!tag.remoteId().isEmpty()) {
                    SelectQueryBuilder<TagRemoteIdResourceRelation> qb;
                    qb.addValueCondition(TagRemoteIdResourceRelation::resourceIdFullColumnName(), Query::Equals, QLatin1String("akonadi_fake_resource_0"));
                    qb.addValueCondition(TagRemoteIdResourceRelation::tagIdColumn(), Query::Equals, actualTag.id());
                    QVERIFY(qb.exec());
                    QCOMPARE(qb.result().size(), 1);
                    QCOMPARE(qb.result()[0].remoteId(), tag.remoteId());
                }
            }

            const QList<Part> actualParts = actualItem.parts().toList();
            QCOMPARE(actualParts.count(), parts.count());
            Q_FOREACH (const FakePart &part, parts) {
                const QList<Part>::const_iterator actualPartIter =
                    std::find_if(actualParts.constBegin(), actualParts.constEnd(),
                                 [part] (Part const &actualPart) {
                                     return part.partType().ns() == actualPart.partType().ns() &&
                                            part.partType().name() == actualPart.partType().name();
                                 });

                QVERIFY(actualPartIter != actualParts.constEnd());
                const Part actualPart = *actualPartIter;
                QVERIFY(actualPart.isValid());
                QCOMPARE(QString::fromUtf8(actualPart.data()), QString::fromUtf8(part.data()));
                QCOMPARE(actualPart.data(), part.data());
                QCOMPARE(actualPart.datasize(), part.datasize());
                QCOMPARE(actualPart.external(), part.external());
            }
        }
    }
};

AKTEST_FAKESERVER_MAIN(AkAppendHandlerTest)

#include "akappendhandlertest.moc"
