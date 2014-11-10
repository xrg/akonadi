/***************************************************************************
 *   Copyright (C) 2006 by Till Adam <adam@kde.org>                        *
 *   Copyright (C) 2013 by Volker Krause <vkrause@kde.org>                 *
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
 *   51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.         *
 ***************************************************************************/
#include "connection.h"

#include <QtCore/QDebug>
#include <QtCore/QEventLoop>
#include <QtCore/QLatin1String>
#include <QSettings>

#include "storage/datastore.h"
#include "handler.h"
#include "response.h"
#include "tracer.h"
#include "clientcapabilityaggregator.h"

#include "imapstreamparser.h"
#include "shared/akdebug.h"
#include "shared/akcrash.h"

#include <akstandarddirs.h>

#include <assert.h>

#define AKONADI_PROTOCOL_VERSION "37"

#define IDLE_TIMER_TIMEOUT 600000 // 5 min

using namespace Akonadi::Server;

Connection::Connection( quintptr socketDescriptor, QObject *parent )
    : QThread( parent )
    , m_socketDescriptor( socketDescriptor )
    , m_socket( 0 )
    , m_currentHandler( 0 )
    , m_connectionState( NonAuthenticated )
    , m_backend( 0 )
    , m_selectedConnection( 0 )
    , m_streamParser( 0 )
    , m_verifyCacheOnRetrieval( false )
    , m_idleTimer(0)
{
    m_identifier.sprintf( "%p", static_cast<void *>( this ) );
    ClientCapabilityAggregator::addSession( m_clientCapabilities );

    const QSettings settings( AkStandardDirs::serverConfigFile(), QSettings::IniFormat );
    m_verifyCacheOnRetrieval = settings.value( QLatin1String( "Cache/VerifyOnRetrieval" ), m_verifyCacheOnRetrieval ).toBool();
    m_idleTimer = new QTimer(this);
}

DataStore *Connection::storageBackend()
{
    if ( !m_backend ) {
      m_backend = DataStore::self();
    }
    return m_backend;
}

Connection::~Connection()
{
    ClientCapabilityAggregator::removeSession( m_clientCapabilities );
    Tracer::self()->endConnection( m_identifier, QString() );
}

void Connection::run()
{
    m_socket = new QLocalSocket();

    if ( !m_socket->setSocketDescriptor( m_socketDescriptor ) ) {
        qWarning() << "Connection(" << m_identifier
                   << ")::run: failed to set socket descriptor: "
                   << m_socket->error() << "(" << m_socket->errorString() << ")";
        delete m_socket;
        m_socket = 0;
        return;
    }

    /* Start a local event loop and start processing incoming data. Whenever
     * a full command has been read, it is delegated to the responsible
     * handler and processed by that. If that command needs to do something
     * asynchronous such as ask the db for data, it returns and the input
     * queue can continue to be processed. Whenever there is something to
     * be sent back to the user it is queued in the form of a Response object.
     * All this is meant to make it possible to process large incoming or
     * outgoing data transfers in a streaming manner, without having to
     * hold them in memory 'en gros'. */

    connect( m_socket, SIGNAL(readyRead()),
             this, SLOT(slotNewData()), Qt::DirectConnection );
    connect( m_socket, SIGNAL(disconnected()),
             this, SLOT(slotDisconnected()), Qt::DirectConnection );
    connect( m_idleTimer, SIGNAL(timeout()),
             this, SLOT(slotConnectionIdle()), Qt::DirectConnection );

    m_streamParser = new ImapStreamParser( m_socket );
    m_streamParser->setTracerIdentifier( m_identifier );

    Response greeting;
    greeting.setUntagged();
    greeting.setString( "OK Akonadi Almost IMAP Server [PROTOCOL " AKONADI_PROTOCOL_VERSION "]" );
    // don't send before the event loop is active, since waitForBytesWritten() can cause interesting reentrancy issues
    // TODO should be QueueConnection, but unfortunately that doesn't work (yet), since
    // "this" belongs to the wrong thread, but that requires a slightly larger refactoring
    QMetaObject::invokeMethod( this, "slotResponseAvailable",
                               Qt::DirectConnection,
                               Q_ARG( Akonadi::Server::Response, greeting ) );

    exec();
    delete m_socket;
    m_socket = 0;
    delete m_streamParser;
    m_streamParser = 0;
}

void Connection::slotDisconnected()
{
    m_idleTimer->stop();
    quit();
}

void Connection::slotConnectionIdle()
{
    if (m_backend && m_backend->isOpened() && !m_currentHandler) {
        akDebug() << "Closing idle db connection" << 
                (m_backend->inTransaction()? " IN TRANSACTION!" : " not in transaction");
        m_backend->close();
        m_backend = 0;
        akDebug() << "Closed idle db connection";
    }
}

void Connection::slotNewData()
{
  // On Windows, calling readLiteralPart() triggers the readyRead() signal recursively and leads to parse errors
  if ( m_currentHandler ) {
    return;
  }

  if (m_idleTimer->isActive())
    m_idleTimer->stop();

  // will only open() a previously idle backend.
  // Otherwise, a new backend could lazily be constructed by later calls.
  if (!DataStore::self()->isOpened()) {
        DataStore::self()->open();
  }

  while ( m_socket->bytesAvailable() > 0 || !m_streamParser->readRemainingData().isEmpty() ) {
    try {
      const QByteArray tag = m_streamParser->readString();
      // deal with stray newlines
      if ( tag.isEmpty() && m_streamParser->atCommandEnd() ) {
        continue;
      }
      const QByteArray command = m_streamParser->readString();
      if ( command.isEmpty() ) {
        throw Akonadi::Server::Exception("empty command");
      }
      Tracer::self()->connectionInput( m_identifier, ( tag + ' ' + command + ' ' + m_streamParser->readRemainingData() ) );
      m_currentHandler = findHandlerForCommand( command );
      assert( m_currentHandler );
      connect( m_currentHandler, SIGNAL(responseAvailable(Akonadi::Server::Response)),
               this, SLOT(slotResponseAvailable(Akonadi::Server::Response)), Qt::DirectConnection );
      connect( m_currentHandler, SIGNAL(connectionStateChange(ConnectionState)),
              this, SLOT(slotConnectionStateChange(ConnectionState)),
              Qt::DirectConnection );
      m_currentHandler->setTag( tag );
      m_currentHandler->setStreamParser( m_streamParser );
      if ( !m_currentHandler->parseStream() ) {
        m_streamParser->skipCurrentCommand();
      }
    } catch ( const Akonadi::Server::HandlerException &e ) {
      m_currentHandler->failureResponse( e.what() );
      try {
        m_streamParser->skipCurrentCommand();
      } catch ( ... ) {}
    } catch ( const Akonadi::Server::Exception &e ) {
      if ( m_currentHandler ) {
        m_currentHandler->failureResponse( QByteArray( e.type() ) + QByteArray( ": " ) + QByteArray( e.what() ) );
      }
      try {
        m_streamParser->skipCurrentCommand();
      } catch ( ... ) {}
    } catch ( ... ) {
      akError() << "Unknown exception caught: " << akBacktrace();
      if ( m_currentHandler ) {
        m_currentHandler->failureResponse( "Unknown exception caught" );
      }
      try {
        m_streamParser->skipCurrentCommand();
      } catch ( ... ) {}
    }
    delete m_currentHandler;
    m_currentHandler = 0;

    if ( m_streamParser->readRemainingData().startsWith( '\n' ) || m_streamParser->readRemainingData().startsWith( "\r\n" ) ) {
      try {
        m_streamParser->readUntilCommandEnd(); //just eat the ending newline
      } catch ( ... ) {}
    }
  }

  // reset, arm the timer
  m_idleTimer->start(IDLE_TIMER_TIMEOUT);
}

void Connection::writeOut( const QByteArray &data )
{
    QByteArray block = data + "\r\n";
    m_socket->write( block );
    m_socket->waitForBytesWritten();

    Tracer::self()->connectionOutput( m_identifier, block );
}

Handler *Connection::findHandlerForCommand( const QByteArray &command )
{
    Handler *handler = Handler::findHandlerForCommandAlwaysAllowed( command );
    if ( handler ) {
      handler->setConnection( this );
      return handler;
    }

    switch ( m_connectionState ) {
    case NonAuthenticated:
        handler =  Handler::findHandlerForCommandNonAuthenticated( command );
        break;
    case Authenticated:
        handler =  Handler::findHandlerForCommandAuthenticated( command, m_streamParser );
        break;
    case Selected:
        break;
    case LoggingOut:
        break;
    }
    // we didn't have a handler for this, let the default one do its thing
    if ( !handler ) {
        handler = new UnknownCommandHandler( command );
    }
    handler->setConnection( this );
    return handler;
}

void Connection::slotResponseAvailable( const Response &response )
{
    // FIXME handle reentrancy in the presence of continuation. Something like:
    // "if continuation pending, queue responses, once continuation is done, replay them"
    writeOut( response.asString() );
}

void Connection::slotConnectionStateChange( ConnectionState state )
{
    if ( state == m_connectionState ) {
        return;
    }
    m_connectionState = state;
    switch ( m_connectionState ) {
    case NonAuthenticated:
        assert( 0 ); // can't happen, it's only the initial state, we can't go back to it
        break;
    case Authenticated:
        break;
    case Selected:
        break;
    case LoggingOut:
        m_socket->disconnectFromServer();
        break;
    }
}

qint64 Connection::selectedCollectionId() const
{
    return m_selectedConnection;
}

void Connection::setSelectedCollection( qint64 collection )
{
    m_selectedConnection = collection;
}

const Collection Connection::selectedCollection()
{
  return Collection::retrieveById( selectedCollectionId() );
}

void Connection::addStatusMessage( const QByteArray &msg )
{
    m_statusMessageQueue.append( msg );
}

void Connection::flushStatusMessageQueue()
{
    for ( int i = 0; i < m_statusMessageQueue.count(); ++i ) {
      Response response;
      response.setUntagged();
      response.setString( m_statusMessageQueue[i] );

      slotResponseAvailable( response );
    }

    m_statusMessageQueue.clear();
}

void Connection::setSessionId( const QByteArray &id )
{
  m_identifier.sprintf( "%s (%p)", id.data(), static_cast<void *>( this ) );
  Tracer::self()->beginConnection( m_identifier, QString() );
  m_streamParser->setTracerIdentifier( m_identifier );

  m_sessionId = id;
  setObjectName( QString::fromLatin1( id ) );
  storageBackend()->setSessionId( id );
  storageBackend()->notificationCollector()->setSessionId( id );
}

QByteArray Connection::sessionId() const
{
  return m_sessionId;
}

Resource Connection::resourceContext() const
{
  return m_resourceContext;
}

void Connection::setResourceContext( const Resource &res )
{
  m_resourceContext = res;
}

bool Connection::isOwnerResource( const PimItem &item ) const
{
  if ( resourceContext().isValid() && item.collection().resourceId() == resourceContext().id() ) {
    return true;
  }
  // fallback for older resources
  if ( sessionId() == item.collection().resource().name().toUtf8() ) {
    return true;
  }
  return false;
}

bool Connection::isOwnerResource( const Collection &collection ) const
{
  if ( resourceContext().isValid() && collection.resourceId() == resourceContext().id() ) {
    return true;
  }
  if ( sessionId() == collection.resource().name().toUtf8() ) {
    return true;
  }
  return false;
}

const ClientCapabilities &Connection::capabilities() const
{
  return m_clientCapabilities;
}

void Connection::setCapabilities( const ClientCapabilities &capabilities )
{
  ClientCapabilityAggregator::removeSession( m_clientCapabilities );
  m_clientCapabilities = capabilities;
  ClientCapabilityAggregator::addSession( m_clientCapabilities );
}

bool Connection::verifyCacheOnRetrieval() const
{
  return m_verifyCacheOnRetrieval;
}
