/*
    Copyright (c) 2007 Volker Krause <vkrause@kde.org>

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

#include "subscribe.h"

#include "../../libs/imapparser_p.h"
#include "imapstreamparser.h"
#include <handlerhelper.h>
#include <akonadiconnection.h>
#include <storage/datastore.h>
#include <storage/transaction.h>

using namespace Akonadi;

bool Subscribe::handleLine(const QByteArray & line)
{
  QByteArray buffer;
  int pos = ImapParser::parseString( line, buffer ); // tag

  // command
  pos = ImapParser::parseString( line, buffer, pos );
  const bool subscribe = buffer == QByteArray( "SUBSCRIBE" );

  DataStore *store = connection()->storageBackend();
  Transaction transaction( store );

  forever {
    pos = ImapParser::parseString( line, buffer, pos );
    if ( pos == line.length() || buffer.isEmpty() )
      break;
    Collection col = HandlerHelper::collectionFromIdOrName( buffer );
    if ( !col.isValid() )
      return failureResponse( "Invalid collection" );
    if ( col.subscribed() == subscribe )
      continue;
    // TODO do all changes in one db operation
    col.setSubscribed( subscribe );
    if ( !col.update() )
      return failureResponse( "Unable to change subscription" );
  }

  if ( !transaction.commit() )
    return failureResponse( "Cannot commit transaction." );

  return successResponse( "Completed" );
}

bool Subscribe::supportsStreamParser()
{
  return true;
}

bool Subscribe::parseStream()
{
  qDebug() << "Subscribe::parseStream";
  QByteArray tmp = m_streamParser->readString(); // skip command
  if (tmp != "SUBSCRIBE" && tmp != "UNSUBSCRIBE") {
    //put back what was read
    m_streamParser->insertData(' ' + tmp + ' ');
  }
  const bool subscribe = tmp == QByteArray( "SUBSCRIBE" );

  DataStore *store = connection()->storageBackend();
  Transaction transaction( store );

  QByteArray buffer;
  while (!m_streamParser->atCommandEnd()) {
    buffer = m_streamParser->readString();
    if ( buffer.isEmpty() )
      break;
    Collection col = HandlerHelper::collectionFromIdOrName( buffer );
    if ( !col.isValid() )
      return failureResponse( "Invalid collection" );
    if ( col.subscribed() == subscribe )
      continue;
    // TODO do all changes in one db operation
    col.setSubscribed( subscribe );
    if ( !col.update() )
      return failureResponse( "Unable to change subscription" );
  }

  if ( !transaction.commit() )
    return failureResponse( "Cannot commit transaction." );

  return successResponse( "Completed" );
}


#include "subscribe.moc"
