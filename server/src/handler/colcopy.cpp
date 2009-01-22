/*
    Copyright (c) 2008 Volker Krause <vkrause@kde.org>

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

#include "colcopy.h"

#include "akonadiconnection.h"
#include "handlerhelper.h"
#include "../../libs/imapparser_p.h"
#include "storage/datastore.h"
#include "storage/transaction.h"

using namespace Akonadi;

bool ColCopy::handleLine(const QByteArray & line)
{
  QByteArray tmp;
  int pos = ImapParser::parseString( line, tmp ); // skip tag
  pos = ImapParser::parseString( line, tmp, pos ); // skip command

  pos = ImapParser::parseString( line, tmp, pos );
  const Collection source = HandlerHelper::collectionFromIdOrName( tmp );
  if ( !source.isValid() )
    return failureResponse( "No valid source specified" );

  ImapParser::parseString( line, tmp, pos );
  const Collection target = HandlerHelper::collectionFromIdOrName( tmp );
  if ( !target.isValid() )
    return failureResponse( "No valid target specified" );

  DataStore *store = connection()->storageBackend();
  Transaction transaction( store );

  if ( !copyCollection( source, target ) )
    return failureResponse( "Failed to copy collection" );

  if ( !transaction.commit() )
    return failureResponse( "Cannot commit transaction." );

  return successResponse( "COLCOPY complete" );
}

bool ColCopy::copyCollection(const Collection & source, const Collection & target)
{
  // copy the source collection
  Collection col = source;
  col.setParentId( target.id() );
  col.setResourceId( target.resourceId() );
  DataStore *db = connection()->storageBackend();
  if ( !db->appendCollection( col ) )
    return false;

  foreach ( const MimeType &mt, source.mimeTypes() ) {
    if ( !col.addMimeType( mt ) )
      return false;
  }

  foreach ( const CollectionAttribute &attr, source.attributes() ) {
    CollectionAttribute newAttr = attr;
    newAttr.setId( -1 );
    newAttr.setCollectionId( col.id() );
    if ( !newAttr.insert() )
      return false;
  }

  // copy sub-collections
  foreach ( const Collection &child, source.children() ) {
    if ( !copyCollection( child, col ) )
      return false;
  }

  // copy items
  foreach ( const PimItem &item, source.items() ) {
    if ( !copyItem( item, col ) )
      return false;
  }

  return true;
}
