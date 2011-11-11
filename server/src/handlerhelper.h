/***************************************************************************
 *   Copyright (C) 2006 by Tobias Koenig <tokoe@kde.org>                   *
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

#ifndef AKONADIHANDLERHELPER_H
#define AKONADIHANDLERHELPER_H

#include <entities.h>

#include <QtCore/QByteArray>
#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QStack>

namespace Akonadi {

class ImapStreamParser;

/**
  Helper functions for command handlers.
*/
class HandlerHelper
{
  public:
    /**
      Returns the collection identified by the given id or path.
    */
    static Collection collectionFromIdOrName( const QByteArray &id );

    /**
      Returns the full path for the given collection.
    */
    static QString pathForCollection( const Collection &col );

    /**
      Returns the amount of existing items in the given collection.
      @return -1 on error
    */
    static int itemCount( const Collection &col );

    /**
     * Queries for collection statistics.
     * @param col The collection to query.
     * @param count The total amount of items in this collection.
     * @param size The size of all items in this collection.
     * @return @c false on a query error, @c true otherwise
     */
    static bool itemStatistics( const Collection &col, qint64 &count, qint64 &size );

    /**
      Returns the amount of existing items in the given collection
      which have a given flag set.
      @return -1 on error.
    */
    static int itemWithFlagsCount( const Collection &col, const QStringList &flags );

    /**
      Parse cache policy and update the given Collection object accoordingly.
      @param changed Indicates whether or not the cache policy already available in @p col
      has actually changed
      @todo Error handling.
    */
    static int parseCachePolicy( const QByteArray& data, Akonadi::Collection& col, int start = 0, bool* changed = 0 );

    /**
      Returns the protocol representation of the cache policy of the given
      Collection object.
    */
    static QByteArray cachePolicyToByteArray( const Collection &col );

    /**
      Returns the protocol representation of the given collection.
      Make sure DataStore::activeCachePolicy() has been called before to include
      the effective cache policy
    */
    static QByteArray collectionToByteArray( const Collection &col, bool hidden = false, bool includeStatistics = false,
                                             int ancestorDepth = 0, const QStack<Collection> &ancestors = QStack<Collection>() );

    /**
      Returns the protocol representation of a collection ancestor chain.
    */
    static QByteArray ancestorsToByteArray( int ancestorDepth, const QStack<Collection> &ancestors );

    /**
      Parses the listing/ancestor depth parameter.
    */
    static int parseDepth( const QByteArray &depth );

    /**
      Converts a bytearray list of flag names into flag records.
      @throws HandlerException on errors during datbase operations
    */
    static Akonadi::Flag::List resolveFlags( const QList<QByteArray> &flagNames );
};

}

#endif
