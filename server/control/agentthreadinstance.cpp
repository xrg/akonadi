/*
    Copyright (c) 2010 Bertjan Broeksema <broeksema@kde.org>

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
#include "agentthreadinstance.h"

#include "agentserverinterface.h"
#include "agenttype.h"
#include "shared/akdebug.h"

#include <QtDBus/QDBusServiceWatcher>

using namespace Akonadi;

AgentThreadInstance::AgentThreadInstance( AgentManager *manager )
  : AgentInstance( manager )
{
  QDBusServiceWatcher *watcher = new QDBusServiceWatcher( QLatin1String( "org.freedesktop.Akonadi.AgentServer" ),
                                                          QDBusConnection::sessionBus(),
                                                          QDBusServiceWatcher::WatchForRegistration, this );
  connect( watcher, SIGNAL( serviceRegistered( const QString& ) ),
           this, SLOT( agentServerRegistered() ) );
}

bool AgentThreadInstance::start( const AgentType &agentInfo )
{
  Q_ASSERT( !identifier().isEmpty() );
  if ( identifier().isEmpty() )
    return false;

  setAgentType( agentInfo.identifier );
  mAgentType = agentInfo;

  org::freedesktop::Akonadi::AgentServer agentServer( "org.freedesktop.Akonadi.AgentServer",
                                                      "/AgentServer", QDBusConnection::sessionBus() );
  if ( !agentServer.isValid() ) {
    akDebug() << "AgentServer not up (yet?)";
    return false;
  }

  // TODO: let startAgent return a bool.
  agentServer.startAgent( identifier(), agentInfo.identifier, agentInfo.exec );
  return true;
}

void AgentThreadInstance::quit()
{
  AgentInstance::quit();

  org::freedesktop::Akonadi::AgentServer agentServer( "org.freedesktop.Akonadi.AgentServer",
                                                      "/AgentServer", QDBusConnection::sessionBus() );
  agentServer.stopAgent( identifier() );
}

void AgentThreadInstance::restartWhenIdle()
{
  if ( status() == 0 && !identifier().isEmpty() ) {
    org::freedesktop::Akonadi::AgentServer agentServer( "org.freedesktop.Akonadi.AgentServer",
                                                        "/AgentServer", QDBusConnection::sessionBus() );
    agentServer.stopAgent( identifier() );
    agentServer.startAgent( identifier(), agentType(), mAgentType.exec );
  }
}

void AgentThreadInstance::agentServerRegistered()
{
  start( mAgentType );
}

void Akonadi::AgentThreadInstance::configure(qlonglong windowId)
{
  org::freedesktop::Akonadi::AgentServer agentServer( "org.freedesktop.Akonadi.AgentServer",
                                                      "/AgentServer", QDBusConnection::sessionBus() );
  agentServer.agentInstanceConfigure( identifier(), windowId );
}
