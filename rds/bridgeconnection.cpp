/***************************************************************************
 *   Copyright (C) 2010 by Volker Krause <vkrause@kde.org>                 *
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

#include "bridgeconnection.h"

#include <libs/xdgbasedirs_p.h>

#include <QMetaObject>
#include <QTcpSocket>
#include <QLocalSocket>
#include <QSettings>
#include <QRegExp>

#ifdef Q_OS_UNIX
#include <sys/socket.h>
#include <sys/un.h>
#endif

BridgeConnection::BridgeConnection( QTcpSocket* remoteSocket, QObject *parent) :
    QObject(parent),
    m_localSocket( new QLocalSocket( this ) ),
    m_remoteSocket( remoteSocket )
{
  connect( m_localSocket, SIGNAL(disconnected()), SLOT(deleteLater()) );
  connect( m_remoteSocket, SIGNAL(disconnected()), SLOT(deleteLater()) );
  connect( m_localSocket, SIGNAL(readyRead()), SLOT(slotDataAvailable()) );
  connect( m_remoteSocket, SIGNAL(readyRead()), SLOT(slotDataAvailable()) );
  connect( m_localSocket, SIGNAL(connected()), SLOT(slotDataAvailable()) );

  // wait for the vtable to be complete
  QMetaObject::invokeMethod( this, "connectLocal", Qt::QueuedConnection );
}

BridgeConnection::~BridgeConnection()
{
  delete m_remoteSocket;
}

void BridgeConnection::slotDataAvailable()
{
  if ( m_localSocket->bytesAvailable() > 0 )
    m_remoteSocket->write( m_localSocket->read( m_localSocket->bytesAvailable() ) );
  if ( m_remoteSocket->bytesAvailable() > 0 )
    m_localSocket->write( m_remoteSocket->read( m_remoteSocket->bytesAvailable() ) );
}

void AkonadiBridgeConnection::connectLocal()
{
  const QSettings connectionSettings( Akonadi::XdgBaseDirs::akonadiConnectionConfigFile(), QSettings::IniFormat );
#ifdef Q_OS_WIN  //krazy:exclude=cpp
    const QString namedPipe = connectionSettings.value( QLatin1String( "Data/NamedPipe" ), QLatin1String( "Akonadi" ) ).toString();
    m_localSocket->connectToServer( namedPipe );
#else
    const QString defaultSocketDir = Akonadi::XdgBaseDirs::saveDir( "data", QLatin1String( "akonadi" ) );
    const QString path = connectionSettings.value( QLatin1String( "Data/UnixPath" ), defaultSocketDir + QLatin1String( "/akonadiserver.socket" ) ).toString();
    m_localSocket->connectToServer( path );
#endif
}

void DBusBridgeConnection::connectLocal()
{
  // TODO: support for !Linux
#ifdef Q_OS_UNIX
  const QByteArray sessionBusAddress = qgetenv( "DBUS_SESSION_BUS_ADDRESS" );
  QRegExp rx( "=(.*)[,$]" );
  if ( rx.indexIn( QString::fromLatin1( sessionBusAddress ) ) >= 0 ) {
    const QString dbusPath = rx.cap( 1 );
    qDebug() << dbusPath;
    if ( sessionBusAddress.contains( "abstract" ) ) {
      const int fd = socket( PF_UNIX, SOCK_STREAM, 0 );
      Q_ASSERT( fd >= 0 );
      struct sockaddr_un dbus_socket_addr;
      dbus_socket_addr.sun_family = PF_UNIX;
      dbus_socket_addr.sun_path[0] = '\0'; // this marks an abstract unix socket on linux, something QLocalSocket doesn't support
      memcpy( dbus_socket_addr.sun_path + 1, dbusPath.toLatin1().data(), dbusPath.toLatin1().size() + 1);
      /*sizeof(dbus_socket_addr) gives me a too large value for some reason, although that's what QLocalSocket uses*/
      const int result = ::connect(fd, (struct sockaddr *)&dbus_socket_addr, sizeof(dbus_socket_addr.sun_family) + dbusPath.size() + 1 /* for the leading \0 */);
      Q_ASSERT( result != -1 );
      m_localSocket->setSocketDescriptor( fd, QLocalSocket::ConnectedState, QLocalSocket::ReadWrite );
    } else {
      m_localSocket->connectToServer( dbusPath );
    }
  }
#endif
}