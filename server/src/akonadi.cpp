/***************************************************************************
 *   Copyright (C) 2006 by Till Adam <adam@kde.org>                        *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU Library General Public License as       *
 *   published by the Free Software Foundation; either version 2 of the    *
 *   License, or (at your option) any later version.                       *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this program; if not, write to the                 *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.             *
 ***************************************************************************/

#include "akonadi.h"
#include "connectionthread.h"
#include "serveradaptor.h"
#include <akdbus.h>
#include <akdebug.h>
#include <akstandarddirs.h>

#include "cachecleaner.h"
#include "intervalcheck.h"
#include "storagejanitor.h"
#include "storage/dbconfig.h"
#include "storage/datastore.h"
#include "notificationmanager.h"
#include "resourcemanager.h"
#include "tracer.h"
#include "utils.h"
#include "debuginterface.h"
#include "storage/itemretrievalthread.h"
#include "storage/collectionstatistics.h"
#include "preprocessormanager.h"
#include "search/searchmanager.h"
#include "search/searchtaskmanagerthread.h"
#include "response.h"
#include "collectionreferencemanager.h"

#include "libs/xdgbasedirs_p.h"
#include "libs/protocol_p.h"

#include <QtSql/QSqlQuery>
#include <QtSql/QSqlError>

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QSettings>
#include <QtCore/QTimer>
#include <QtDBus/QDBusServiceWatcher>

#include <config-akonadi.h>
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif
#include <stdlib.h>

#ifdef Q_WS_WIN
#include <windows.h>
#include <Sddl.h>
#endif

using namespace Akonadi;
using namespace Akonadi::Server;

AkonadiServer* AkonadiServer::s_instance = 0;

AkonadiServer::AkonadiServer( QObject *parent )
    : QLocalServer( parent )
    , mCacheCleaner( 0 )
    , mIntervalChecker( 0 )
    , mStorageJanitor( 0 )
    , mItemRetrievalThread( 0 )
    , mDatabaseProcess( 0 )
    , mAlreadyShutdown( false )
{
}

bool AkonadiServer::init()
{
    qRegisterMetaType<Akonadi::Server::Response>();

    const QString serverConfigFile = AkStandardDirs::serverConfigFile( XdgBaseDirs::ReadWrite );
    QSettings settings( serverConfigFile, QSettings::IniFormat );
    // Restrict permission to 600, as the file might contain database password in plaintext
    QFile::setPermissions( serverConfigFile, QFile::ReadOwner | QFile::WriteOwner );

    if ( DbConfig::configuredDatabase()->useInternalServer() ) {
        startDatabaseProcess();
    } else {
        createDatabase();
    }

    DbConfig::configuredDatabase()->setup();

    s_instance = this;

    const QString connectionSettingsFile = AkStandardDirs::connectionConfigFile( XdgBaseDirs::WriteOnly );
    QSettings connectionSettings( connectionSettingsFile, QSettings::IniFormat );

#ifdef Q_OS_WIN
    HANDLE hToken = NULL;
    PSID sid;
    QString userID;

    OpenProcessToken( GetCurrentProcess(), TOKEN_READ, &hToken );
    if ( hToken ) {
        DWORD size;
        PTOKEN_USER userStruct;

        GetTokenInformation( hToken, TokenUser, NULL, 0, &size );
        if ( ERROR_INSUFFICIENT_BUFFER == GetLastError() ) {
            userStruct = reinterpret_cast<PTOKEN_USER>( new BYTE[size] );
            GetTokenInformation( hToken, TokenUser, userStruct, size, &size );

            int sidLength = GetLengthSid( userStruct->User.Sid );
            sid = (PSID) malloc( sidLength );
            CopySid( sidLength, sid, userStruct->User.Sid );
            CloseHandle( hToken );
            delete [] userStruct;
        }

        LPWSTR s;
        if ( !ConvertSidToStringSidW( sid, &s ) ) {
            akError() << "Could not determine user id for current process.";
            userID = QString();
        } else {
            userID = QString::fromUtf16( reinterpret_cast<ushort *>( s ) );
            LocalFree( s );
        }
        free( sid );
    }
    QString defaultPipe = QLatin1String( "Akonadi-" ) + userID;

    QString namedPipe = settings.value( QLatin1String( "Connection/NamedPipe" ), defaultPipe ).toString();
    if ( !listen( namedPipe ) ) {
        akFatal() << "Unable to listen on Named Pipe" << namedPipe;
    }

    connectionSettings.setValue( QLatin1String( "Data/Method" ), QLatin1String( "NamedPipe" ) );
    connectionSettings.setValue( QLatin1String( "Data/NamedPipe" ), namedPipe );
#else
    const QString socketDir = Utils::preferredSocketDirectory( AkStandardDirs::saveDir( "data" ) );
    const QString socketFile = socketDir + QLatin1String( "/akonadiserver.socket" );
    unlink( socketFile.toUtf8().constData() );
    if ( !listen( socketFile ) ) {
        akFatal() << "Unable to listen on Unix socket" << socketFile;
    }

    connectionSettings.setValue( QLatin1String( "Data/Method" ), QLatin1String( "UnixPath" ) );
    connectionSettings.setValue( QLatin1String( "Data/UnixPath" ), socketFile );
#endif

    // initialize the database
    DataStore *db = DataStore::self();
    if ( !db->database().isOpen() ) {
        akFatal() << "Unable to open database" << db->database().lastError().text();
    }
    if ( !db->init() ) {
        akFatal() << "Unable to initialize database.";
    }

    NotificationManager::self();
    Tracer::self();
    new DebugInterface( this );
    ResourceManager::self();

    CollectionStatistics::self();

    // Initialize the preprocessor manager
    PreprocessorManager::init();

    // Forcibly disable it if configuration says so
    if ( settings.value( QLatin1String( "General/DisablePreprocessing" ), false ).toBool() ) {
        PreprocessorManager::instance()->setEnabled( false );
    }

    if ( settings.value( QLatin1String( "Cache/EnableCleaner" ), true ).toBool() ) {
        mCacheCleaner = new CacheCleaner( this );
        mCacheCleaner->start( QThread::IdlePriority );
    }

    mIntervalChecker = new IntervalCheck( this );
    mIntervalChecker->start( QThread::IdlePriority );

    mStorageJanitor = new StorageJanitorThread;
    mStorageJanitor->start( QThread::IdlePriority );

    mItemRetrievalThread = new ItemRetrievalThread( this );
    mItemRetrievalThread->start( QThread::HighPriority );

    mAgentSearchManagerThread = new SearchTaskManagerThread( this );
    mAgentSearchManagerThread->start();

    const QStringList searchManagers = settings.value( QLatin1String( "Search/Manager" ),
                                                       QStringList() << QLatin1String( "Nepomuk" )
                                                                     << QLatin1String( "Agent" ) ).toStringList();
    mSearchManager = new SearchManagerThread( searchManagers, this );
    mSearchManager->start();

    new ServerAdaptor( this );
    QDBusConnection::sessionBus().registerObject( QLatin1String( "/Server" ), this );

    const QByteArray dbusAddress = qgetenv( "DBUS_SESSION_BUS_ADDRESS" );
    if ( !dbusAddress.isEmpty() ) {
        connectionSettings.setValue( QLatin1String( "DBUS/Address" ), QLatin1String( dbusAddress ) );
    }

    QDBusServiceWatcher *watcher = new QDBusServiceWatcher( AkDBus::serviceName( AkDBus::Control ),
                                                            QDBusConnection::sessionBus(),
                                                            QDBusServiceWatcher::WatchForOwnerChange, this );

    connect( watcher, SIGNAL(serviceOwnerChanged(QString,QString,QString)),
             this, SLOT(serviceOwnerChanged(QString,QString,QString)) );

    // Unhide all the items that are actually hidden.
    // The hidden flag was probably left out after an (abrupt)
    // server quit. We don't attempt to resume preprocessing
    // for the items as we don't actually know at which stage the
    // operation was interrupted...
    db->unhideAllPimItems();

    // Cleanup referenced collections from the last run
    CollectionReferenceManager::cleanup();

    // We are ready, now register org.freedesktop.Akonadi service to DBus and
    // the fun can begin
    if ( !QDBusConnection::sessionBus().registerService( AkDBus::serviceName( AkDBus::Server ) ) ) {
        akFatal() << "Unable to connect to dbus service: " << QDBusConnection::sessionBus().lastError().message();
    }

    return true;
}

AkonadiServer::~AkonadiServer()
{
}

template <typename T> static void quitThread( T &thread )
{
    if ( !thread ) {
        return;
    }
    thread->quit();
    thread->wait();
    delete thread;
    thread = 0;
}

bool AkonadiServer::quit()
{
    if ( mAlreadyShutdown ) {
        return true;
    }
    mAlreadyShutdown = true;

    akDebug() << "terminating service threads";
    quitThread( mCacheCleaner );
    quitThread( mIntervalChecker );
    quitThread( mStorageJanitor );
    quitThread( mItemRetrievalThread );
    mAgentSearchManagerThread->stop();
    quitThread( mAgentSearchManagerThread );
    quitThread( mSearchManager );

    delete mSearchManager;
    mSearchManager = 0;

    akDebug() << "terminating connection threads";
    for ( int i = 0; i < mConnections.count(); ++i ) {
        quitThread( mConnections[i] );
    }
    mConnections.clear();

    // Terminate the preprocessor manager before the database but after all connections are gone
    PreprocessorManager::done();

    DataStore::self()->close();

    akDebug() << "stopping db process";
    stopDatabaseProcess();

    QSettings settings( AkStandardDirs::serverConfigFile(), QSettings::IniFormat );
    const QString connectionSettingsFile = AkStandardDirs::connectionConfigFile( XdgBaseDirs::WriteOnly );

#ifndef Q_OS_WIN
    const QString socketDir = Utils::preferredSocketDirectory( AkStandardDirs::saveDir( "data" ) );

    if ( !QDir::home().remove( socketDir + QLatin1String( "/akonadiserver.socket" ) ) ) {
        akError() << "Failed to remove Unix socket";
    }
#endif
    if ( !QDir::home().remove( connectionSettingsFile ) ) {
        akError() << "Failed to remove runtime connection config file";
    }

    QTimer::singleShot( 0, this, SLOT(doQuit()) );

    return true;
}

void AkonadiServer::doQuit()
{
    QCoreApplication::exit();
}

void AkonadiServer::incomingConnection( quintptr socketDescriptor )
{
    if ( mAlreadyShutdown ) {
        return;
    }
    QPointer<ConnectionThread> thread = new ConnectionThread( socketDescriptor, this );
    connect( thread, SIGNAL(finished()), thread, SLOT(deleteLater()) );
    mConnections.append( thread );
    thread->start();
}

AkonadiServer *AkonadiServer::instance()
{
    if ( !s_instance ) {
        s_instance = new AkonadiServer();
    }
    return s_instance;
}

void AkonadiServer::startDatabaseProcess()
{
    if ( !DbConfig::configuredDatabase()->useInternalServer() ) {
        return;
    }

    // create the database directories if they don't exists
    AkStandardDirs::saveDir( "data" );
    AkStandardDirs::saveDir( "data", QLatin1String( "file_db_data" ) );

    DbConfig::configuredDatabase()->startInternalServer();
}

void AkonadiServer::createDatabase()
{
    const QLatin1String initCon( "initConnection" );
    QSqlDatabase db = QSqlDatabase::addDatabase( DbConfig::configuredDatabase()->driverName(), initCon );
    DbConfig::configuredDatabase()->apply( db );
    db.setDatabaseName( DbConfig::configuredDatabase()->databaseName() );
    if ( !db.isValid() ) {
        akFatal() << "Invalid database object during initial database connection";
    }

    if ( db.open() ) {
        db.close();
    } else {
        akDebug() << "Failed to use database" << DbConfig::configuredDatabase()->databaseName();
        akDebug() << "Database error:" << db.lastError().text();
        akDebug() << "Trying to create database now...";

        db.close();
        db.setDatabaseName( QString() );
        if ( db.open() ) {
            {
                QSqlQuery query( db );
                if ( !query.exec( QString::fromLatin1( "CREATE DATABASE %1" ).arg( DbConfig::configuredDatabase()->databaseName() ) ) ) {
                    akError() << "Failed to create database";
                    akError() << "Query error:" << query.lastError().text();
                    akFatal() << "Database error:" << db.lastError().text();
                }
            } // make sure query is destroyed before we close the db
            db.close();
        }
    }
    QSqlDatabase::removeDatabase( initCon );
}

void AkonadiServer::stopDatabaseProcess()
{
    if ( !DbConfig::configuredDatabase()->useInternalServer() ) {
        return;
    }

    DbConfig::configuredDatabase()->stopInternalServer();
}

void AkonadiServer::serviceOwnerChanged( const QString &, const QString &oldOwner, const QString &newOwner )
{
    Q_UNUSED( oldOwner );
    if ( newOwner.isEmpty() ) {
        akError() << "Control process died, committing suicide!";
        quit();
    }
}

CacheCleaner* AkonadiServer::cacheCleaner()
{
    return mCacheCleaner;
}

IntervalCheck* AkonadiServer::intervalChecker()
{
    return mIntervalChecker;
}

