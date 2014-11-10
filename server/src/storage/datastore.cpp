/***************************************************************************
 *   Copyright (C) 2006 by Andreas Gungl <a.gungl@gmx.de>                  *
 *   Copyright (C) 2007 by Robert Zwerus <arzie@dds.nl>                    *
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

#include "datastore.h"

#include "dbconfig.h"
#include "dbinitializer.h"
#include "dbupdater.h"
#include "notificationmanager.h"
#include "tracer.h"
#include "transaction.h"
#include "selectquerybuilder.h"
#include "handlerhelper.h"
#include "countquerybuilder.h"
#include "xdgbasedirs_p.h"
#include "akdebug.h"
#include "parthelper.h"
#include "libs/protocol_p.h"
#include "handler.h"
#include "collectionqueryhelper.h"
#include "akonadischema.h"
#include "parttypehelper.h"
#include "querycache.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QEventLoop>
#include <QtCore/QSettings>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QThread>
#include <QtCore/QThreadStorage>
#include <QtCore/QTimer>
#include <QtCore/QUuid>
#include <QtCore/QVariant>
#include <QtSql/QSqlDatabase>
#include <QtSql/QSqlDriver>
#include <QtSql/QSqlError>
#include <QtSql/QSqlField>
#include <QtSql/QSqlQuery>

using namespace Akonadi::Server;

static QMutex sTransactionMutex;
bool DataStore::s_hasForeignKeyConstraints = false;

#define TRANSACTION_MUTEX_LOCK if ( DbType::type( m_database ) == DbType::Sqlite ) sTransactionMutex.lock()
#define TRANSACTION_MUTEX_UNLOCK if ( DbType::type( m_database ) == DbType::Sqlite ) sTransactionMutex.unlock()

/***************************************************************************
 *   DataStore                                                           *
 ***************************************************************************/
DataStore::DataStore()
  : QObject()
  , m_dbOpened( false )
  , m_transactionLevel( 0 )
  , mNotificationCollector( new NotificationCollector( this ) )
  , m_keepAliveTimer( 0 )
{
  open();

  NotificationManager::self()->connectNotificationCollector( mNotificationCollector );

  if ( DbConfig::configuredDatabase()->driverName() == QLatin1String( "QMYSQL" ) ) {
    // Send a dummy query to MySQL every 1 hour to keep the connection alive,
    // otherwise MySQL just drops the connection and our subsequent queries fail
    // without properly reporting the error
    m_keepAliveTimer = new QTimer( this );
    m_keepAliveTimer->setInterval( 3600 * 1000 );
    QObject::connect( m_keepAliveTimer, SIGNAL(timeout()),
                      this, SLOT(sendKeepAliveQuery()) );
    m_keepAliveTimer->start();
  }
}

DataStore::~DataStore()
{
  if (m_dbOpened)
    close();
}

void DataStore::open()
{
  m_connectionName = QUuid::createUuid().toString() + QString::number( reinterpret_cast<qulonglong>( QThread::currentThread() ) );
  Q_ASSERT( !QSqlDatabase::contains( m_connectionName ) );

  m_database = QSqlDatabase::addDatabase( DbConfig::configuredDatabase()->driverName(), m_connectionName );
  DbConfig::configuredDatabase()->apply( m_database );

  if ( !m_database.isValid() ) {
    m_dbOpened = false;
    return;
  }
  m_dbOpened = m_database.open();

  if ( !m_dbOpened ) {
    debugLastDbError( "Cannot open database." );
  } else {
    akDebug() << "Database" << m_database.databaseName() << "opened using driver" << m_database.driverName();
  }

  DbConfig::configuredDatabase()->initSession( m_database );
}

QSqlDatabase DataStore::database()
{
    if (!m_dbOpened)
        open();
    return m_database;
}

void DataStore::close()
{

  if ( m_keepAliveTimer ) {
    m_keepAliveTimer->stop();
  }

  if ( !m_dbOpened ) {
    return;
  }

  if ( inTransaction() ) {
    // By setting m_transactionLevel to '1' here, we skip all nested transactions
    // and rollback the outermost transaction.
    m_transactionLevel = 1;
    rollbackTransaction();
  }

  QueryCache::clear();
  m_database.close();
  m_database = QSqlDatabase();
  QSqlDatabase::removeDatabase( m_connectionName );
  m_transactionQueries.clear();

  m_dbOpened = false;
}

bool DataStore::init()
{
  Q_ASSERT( QThread::currentThread() == QCoreApplication::instance()->thread() );

  AkonadiSchema schema;
  DbInitializer::Ptr initializer = DbInitializer::createInstance( m_database, &schema );
  if ( !initializer->run() ) {
    akError() << initializer->errorMsg();
    return false;
  }
  s_hasForeignKeyConstraints = initializer->hasForeignKeyConstraints();

  DbUpdater updater( m_database, QLatin1String( ":dbupdate.xml" ) );
  if ( !updater.run() ) {
    return false;
  }

  if ( !initializer->updateIndexesAndConstraints() ) {
    akError() << initializer->errorMsg();
    return false;
  }

  // enable caching for some tables
  MimeType::enableCache( true );
  Flag::enableCache( true );
  Resource::enableCache( true );
  Collection::enableCache( true );

  return true;
}

QThreadStorage<DataStore *> instances;

DataStore *DataStore::self()
{
  if ( !instances.hasLocalData() ) {
    instances.setLocalData( new DataStore() );
  }
  return instances.localData();
}

/* --- ItemFlags ----------------------------------------------------- */

bool DataStore::setItemsFlags( const PimItem::List &items, const QVector<Flag> &flags )
{
  QSet<QByteArray> removedFlags;
  QSet<QByteArray> addedFlags;
  QVariantList insIds;
  QVariantList insFlags;
  Query::Condition delConds( Query::Or );

  Q_FOREACH ( const PimItem &item, items ) {
    Q_FOREACH ( const Flag &flag, item.flags() ) {
      if ( !flags.contains( flag ) ) {
        removedFlags << flag.name().toLatin1();
        Query::Condition cond;
        cond.addValueCondition( PimItemFlagRelation::leftFullColumnName(), Query::Equals, item.id() );
        cond.addValueCondition( PimItemFlagRelation::rightFullColumnName(), Query::Equals, flag.id() );
        delConds.addCondition(cond);
      }
    }

    Q_FOREACH ( const Flag &flag, flags ) {
      if ( !item.flags().contains( flag ) ) {
        addedFlags << flag.name().toLatin1();
        insIds << item.id();
        insFlags << flag.id();
      }
    }
  }

  if ( !removedFlags.empty() ) {
    QueryBuilder qb( PimItemFlagRelation::tableName(), QueryBuilder::Delete );
    qb.addCondition( delConds );
    if ( !qb.exec() ) {
      return false;
    }
  }

  if ( !addedFlags.empty() ) {
    QueryBuilder qb2( PimItemFlagRelation::tableName(), QueryBuilder::Insert );
    qb2.setColumnValue( PimItemFlagRelation::leftColumn(), insIds );
    qb2.setColumnValue( PimItemFlagRelation::rightColumn(), insFlags );
    qb2.setIdentificationColumn( QString() );
    if ( !qb2.exec() ) {
      return false;
    }
  }

  if ( addedFlags.empty() && removedFlags.empty() ) {
    // no changes done, notification not needed
    return true;
  }

  mNotificationCollector->itemsFlagsChanged( items, addedFlags, removedFlags );

  return true;
}

bool DataStore::doAppendItemsFlag( const PimItem::List &items, const Flag &flag,
                                   const QSet<Entity::Id> &existing, const Collection &col,
                                   bool silent )
{
  QVariantList flagIds;
  QVariantList appendIds;
  PimItem::List appendItems;
  Q_FOREACH ( const PimItem &item, items ) {
    if ( existing.contains( item.id() ) ) {
      continue;
    }

    flagIds << flag.id();
    appendIds << item.id();
    appendItems << item;
  }

  if ( appendItems.isEmpty() ) {
    return true; // all items have the desired flags already
  }

  QueryBuilder qb2( PimItemFlagRelation::tableName(), QueryBuilder::Insert );
  qb2.setColumnValue( PimItemFlagRelation::leftColumn(), appendIds );
  qb2.setColumnValue( PimItemFlagRelation::rightColumn(), flagIds );
  qb2.setIdentificationColumn( QString() );
  if ( !qb2.exec() ) {
    akDebug() << "Failed to execute query:" << qb2.query().lastError();
    return false;
  }

  if ( !silent ) {
    mNotificationCollector->itemsFlagsChanged( appendItems, QSet<QByteArray>() << flag.name().toLatin1(),
                                               QSet<QByteArray>(), col );
  }

  return true;
}

bool DataStore::appendItemsFlags( const PimItem::List &items, const QVector<Flag> &flags,
                                  bool &flagsChanged, bool checkIfExists,
                                  const Collection &col, bool silent )
{
  QSet<QByteArray> added;

  QVariantList itemsIds;
  Q_FOREACH ( const PimItem &item, items ) {
    itemsIds.append( item.id() );
  }

  flagsChanged = false;
  Q_FOREACH ( const Flag &flag, flags ) {
    QSet<PimItem::Id> existing;
    if ( checkIfExists ) {
      QueryBuilder qb( PimItemFlagRelation::tableName(), QueryBuilder::Select );
      Query::Condition cond;
      cond.addValueCondition( PimItemFlagRelation::rightColumn(), Query::Equals, flag.id() );
      cond.addValueCondition( PimItemFlagRelation::leftColumn(), Query::In, itemsIds );
      qb.addColumn( PimItemFlagRelation::leftColumn() );
      qb.addCondition( cond );

      if ( !qb.exec() ) {
        akDebug() << "Failed to execute query:" << qb.query().lastError();
        return false;
      }

      QSqlQuery query = qb.query();
      if ( query.size() == items.count() ) {
        continue;
      }

      flagsChanged = true;

      while ( query.next() ) {
        existing << query.value( 0 ).value<PimItem::Id>();
      }
    }

    if ( !doAppendItemsFlag( items, flag, existing, col, silent ) ) {
      return false;
    }
  }

  return true;
}

bool DataStore::removeItemsFlags( const PimItem::List &items, const QVector<Flag> &flags )
{
  QSet<QByteArray> removedFlags;
  QVariantList itemsIds;
  QVariantList flagsIds;
  Q_FOREACH ( const PimItem &item, items ) {
    itemsIds << item.id();
    for ( int i = 0; i < flags.count(); ++i ) {
      const QByteArray flagName = flags[i].name().toLatin1();
      if ( !removedFlags.contains( flagName ) ) {
        flagsIds << flags[i].id();
        removedFlags << flagName;
      }
    }
  }

  // Delete all given flags from all given items in one go
  QueryBuilder qb( PimItemFlagRelation::tableName(), QueryBuilder::Delete );
  Query::Condition cond( Query::And );
  cond.addValueCondition( PimItemFlagRelation::rightFullColumnName(), Query::In, flagsIds );
  cond.addValueCondition( PimItemFlagRelation::leftFullColumnName(), Query::In, itemsIds );
  qb.addCondition( cond );
  if ( !qb.exec() ) {
    return false;
  }

  mNotificationCollector->itemsFlagsChanged( items, QSet<QByteArray>(), removedFlags );
  return true;
}

/* --- ItemTags ----------------------------------------------------- */

bool DataStore::setItemsTags( const PimItem::List &items, const Tag::List &tags )
{
  QSet<qint64> removedTags;
  QSet<qint64> addedTags;
  QVariantList insIds;
  QVariantList insTags;
  Query::Condition delConds( Query::Or );

  Q_FOREACH ( const PimItem &item, items ) {
    Q_FOREACH ( const Tag &tag, item.tags() ) {
      if ( !tags.contains( tag ) ) {
        // Remove tags from items that had it set
        removedTags << tag.id();
        Query::Condition cond;
        cond.addValueCondition( PimItemTagRelation::leftFullColumnName(), Query::Equals, item.id() );
        cond.addValueCondition( PimItemTagRelation::rightFullColumnName(), Query::Equals, tag.id() );
        delConds.addCondition(cond);
      }
    }

    Q_FOREACH ( const Tag &tag, tags ) {
      if ( !item.tags().contains( tag ) ) {
        // Add tags to items that did not have the tag
        addedTags << tag.id();
        insIds << item.id();
        insTags << tag.id();
      }
    }
  }

  if ( !removedTags.empty() ) {
    QueryBuilder qb( PimItemTagRelation::tableName(), QueryBuilder::Delete );
    qb.addCondition( delConds );
    if ( !qb.exec() ) {
      return false;
    }
  }

  if ( !addedTags.empty() ) {
    QueryBuilder qb2( PimItemTagRelation::tableName(), QueryBuilder::Insert );
    qb2.setColumnValue( PimItemTagRelation::leftColumn(), insIds );
    qb2.setColumnValue( PimItemTagRelation::rightColumn(), insTags );
    qb2.setIdentificationColumn( QString() );
    if ( !qb2.exec() ) {
      return false;
    }
  }

  if ( addedTags.empty() && removedTags.empty() ) {
    // no changes done, notification not needed
    return true;
  }

  mNotificationCollector->itemsTagsChanged( items, addedTags, removedTags );

  return true;
}

bool DataStore::doAppendItemsTag( const PimItem::List &items, const Tag &tag,
                                   const QSet<Entity::Id> &existing, const Collection &col )
{
  QVariantList tagIds;
  QVariantList appendIds;
  PimItem::List appendItems;
  Q_FOREACH ( const PimItem &item, items ) {
    if ( existing.contains( item.id() ) ) {
      continue;
    }

    tagIds << tag.id();
    appendIds << item.id();
    appendItems << item;
  }

  if ( appendItems.isEmpty() ) {
    return true; // all items have the desired tags already
  }

  QueryBuilder qb2( PimItemTagRelation::tableName(), QueryBuilder::Insert );
  qb2.setColumnValue( PimItemTagRelation::leftColumn(), appendIds );
  qb2.setColumnValue( PimItemTagRelation::rightColumn(), tagIds );
  qb2.setIdentificationColumn( QString() );
  if ( !qb2.exec() ) {
    akDebug() << "Failed to execute query:" << qb2.query().lastError();
    return false;
  }

  mNotificationCollector->itemsTagsChanged( appendItems, QSet<qint64>() << tag.id(),
                                             QSet<qint64>(), col );

  return true;
}

bool DataStore::appendItemsTags( const PimItem::List &items, const Tag::List &tags,
                                  bool &tagsChanged, bool checkIfExists,
                                  const Collection &col )
{
  QSet<QByteArray> added;

  QVariantList itemsIds;
  Q_FOREACH ( const PimItem &item, items ) {
    itemsIds.append( item.id() );
  }

  tagsChanged = false;
  Q_FOREACH ( const Tag &tag, tags ) {
    QSet<PimItem::Id> existing;
    if ( checkIfExists ) {
      QueryBuilder qb( PimItemTagRelation::tableName(), QueryBuilder::Select );
      Query::Condition cond;
      cond.addValueCondition( PimItemTagRelation::rightColumn(), Query::Equals, tag.id() );
      cond.addValueCondition( PimItemTagRelation::leftColumn(), Query::In, itemsIds );
      qb.addColumn( PimItemTagRelation::leftColumn() );
      qb.addCondition( cond );

      if ( !qb.exec() ) {
        akDebug() << "Failed to execute query:" << qb.query().lastError();
        return false;
      }

      QSqlQuery query = qb.query();
      if ( query.size() == items.count() ) {
        continue;
      }

      tagsChanged = true;

      while ( query.next() ) {
        existing << query.value( 0 ).value<PimItem::Id>();
      } }

    if ( !doAppendItemsTag( items, tag, existing, col ) ) {
      return false;
    }
  }

  return true;
}

bool DataStore::removeItemsTags( const PimItem::List &items, const Tag::List &tags )
{
  QSet<qint64> removedTags;
  QVariantList itemsIds;
  QVariantList tagsIds;
  Q_FOREACH ( const PimItem &item, items ) {
    itemsIds << item.id();
    for ( int i = 0; i < tags.count(); ++i ) {
      const qint64 tagId = tags[i].id();
      if ( !removedTags.contains( tagId ) ) {
        tagsIds << tagId;
        removedTags << tagId;
      }
    }
  }

  // Delete all given tags from all given items in one go
  QueryBuilder qb( PimItemTagRelation::tableName(), QueryBuilder::Delete );
  Query::Condition cond( Query::And );
  cond.addValueCondition( PimItemTagRelation::rightFullColumnName(), Query::In, tagsIds );
  cond.addValueCondition( PimItemTagRelation::leftFullColumnName(), Query::In, itemsIds );
  qb.addCondition( cond );
  if ( !qb.exec() ) {
    return false;
  }

  mNotificationCollector->itemsTagsChanged( items, QSet<qint64>(), removedTags );
  return true;
}

/* --- ItemParts ----------------------------------------------------- */

bool DataStore::removeItemParts( const PimItem &item, const QList<QByteArray> &parts )
{
  SelectQueryBuilder<Part> qb;
  qb.addJoin( QueryBuilder::InnerJoin, PartType::tableName(), Part::partTypeIdFullColumnName(), PartType::idFullColumnName() );
  qb.addValueCondition( Part::pimItemIdFullColumnName(), Query::Equals, item.id() );
  qb.addCondition( PartTypeHelper::conditionFromFqNames( parts ) );

  qb.exec();
  Part::List existingParts = qb.result();
  Q_FOREACH ( Part part, existingParts ) {
    if ( !PartHelper::remove( &part ) ) {
      return false;
    }
  }

  mNotificationCollector->itemChanged( item, parts.toSet() );
  return true;
}

bool DataStore::invalidateItemCache( const PimItem &item )
{
  // find all payload item parts
  SelectQueryBuilder<Part> qb;
  qb.addJoin( QueryBuilder::InnerJoin, PimItem::tableName(), PimItem::idFullColumnName(), Part::pimItemIdFullColumnName() );
  qb.addJoin( QueryBuilder::InnerJoin, PartType::tableName(), Part::partTypeIdFullColumnName(), PartType::idFullColumnName() );
  qb.addValueCondition( Part::pimItemIdFullColumnName(), Query::Equals, item.id() );
  qb.addValueCondition( Part::dataFullColumnName(), Query::IsNot, QVariant() );
  qb.addValueCondition( PartType::nsFullColumnName(), Query::Equals, QLatin1String( "PLD" ) );
  qb.addValueCondition( PimItem::dirtyFullColumnName(), Query::Equals, false );

  if ( !qb.exec() ) {
    return false;
  }

  const Part::List parts = qb.result();
  // clear data field
  Q_FOREACH ( Part part, parts ) {
    if ( !PartHelper::truncate( part ) ) {
      return false;
    }
  }

  return true;
}

/* --- Collection ------------------------------------------------------ */
bool DataStore::appendCollection( Collection &collection )
{
  // no need to check for already existing collection with the same name,
  // a unique index on parent + name prevents that in the database
  if ( !collection.insert() ) {
    return false;
  }

  mNotificationCollector->collectionAdded( collection );
  return true;
}

bool DataStore::cleanupCollection( Collection &collection )
{
  if ( !s_hasForeignKeyConstraints ) {
    return cleanupCollection_slow( collection );
  }

  // db will do most of the work for us, we just deal with notifications and external payload parts here
  Q_ASSERT( s_hasForeignKeyConstraints );

  // collect item deletion notifications
  const PimItem::List items = collection.items();
  const QByteArray resource = collection.resource().name().toLatin1();

  // generate the notification before actually removing the data
  // TODO: we should try to get rid of this, requires client side changes to resources and Monitor though
  mNotificationCollector->itemsRemoved( items, collection, resource );

  // remove all external payload parts
  QueryBuilder qb( Part::tableName(), QueryBuilder::Select );
  qb.addColumn( Part::dataFullColumnName() );
  qb.addJoin( QueryBuilder::InnerJoin, PimItem::tableName(), Part::pimItemIdFullColumnName(), PimItem::idFullColumnName() );
  qb.addJoin( QueryBuilder::InnerJoin, Collection::tableName(), PimItem::collectionIdFullColumnName(), Collection::idFullColumnName() );
  qb.addValueCondition( Collection::idFullColumnName(), Query::Equals, collection.id() );
  qb.addValueCondition( Part::externalFullColumnName(), Query::Equals, true );
  qb.addValueCondition( Part::dataFullColumnName(), Query::IsNot, QVariant() );
  if ( !qb.exec() ) {
    return false;
  }

  try {
    while ( qb.query().next() ) {
      PartHelper::removeFile( PartHelper::resolveAbsolutePath( qb.query().value( 0 ).value<QByteArray>() ) );
    }
  } catch ( const PartHelperException &e ) {
    akDebug() << e.what();
    return false;
  }

  // delete the collection itself, referential actions will do the rest
  mNotificationCollector->collectionRemoved( collection );
  return collection.remove();
}

bool DataStore::cleanupCollection_slow( Collection &collection )
{
  Q_ASSERT( !s_hasForeignKeyConstraints );

  // delete the content
  const PimItem::List items = collection.items();
  const QByteArray resource = collection.resource().name().toLatin1();
  mNotificationCollector->itemsRemoved( items, collection, resource );

  Q_FOREACH ( const PimItem &item, items ) {
    if ( !item.clearFlags() ) { // TODO: move out of loop and use only a single query
      return false;
    }
    if ( !PartHelper::remove( Part::pimItemIdColumn(), item.id() ) ) { // TODO: reduce to single query
      return false;
    }

    if ( !PimItem::remove( PimItem::idColumn(), item.id() ) ) { // TODO: move into single query
      return false;
    }

    if ( !Entity::clearRelation<CollectionPimItemRelation>( item.id(), Entity::Right ) ) { // TODO: move into single query
      return false;
    }
  }

  // delete collection mimetypes
  collection.clearMimeTypes();
  Collection::clearPimItems( collection.id() );

  // delete attributes
  Q_FOREACH ( CollectionAttribute attr, collection.attributes() ) {
    if ( !attr.remove() ) {
      return false;
    }
  }

  // delete the collection itself
  mNotificationCollector->collectionRemoved( collection );
  return collection.remove();
}

static bool recursiveSetResourceId( const Collection &collection, qint64 resourceId )
{
  Transaction transaction( DataStore::self() );

  QueryBuilder qb( Collection::tableName(), QueryBuilder::Update );
  qb.addValueCondition( Collection::parentIdColumn(), Query::Equals, collection.id() );
  qb.setColumnValue( Collection::resourceIdColumn(), resourceId );
  qb.setColumnValue( Collection::remoteIdColumn(), QVariant() );
  qb.setColumnValue( Collection::remoteRevisionColumn(), QVariant() );
  if ( !qb.exec() ) {
    return false;
  }

  // this is a cross-resource move, so also reset any resource-specific data (RID, RREV, etc)
  // as well as mark the items dirty to prevent cache purging before they have been written back
  qb = QueryBuilder( PimItem::tableName(), QueryBuilder::Update );
  qb.addValueCondition( PimItem::collectionIdColumn(), Query::Equals, collection.id() );
  qb.setColumnValue( PimItem::remoteIdColumn(), QVariant() );
  qb.setColumnValue( PimItem::remoteRevisionColumn(), QVariant() );
  const QDateTime now = QDateTime::currentDateTime();
  qb.setColumnValue( PimItem::datetimeColumn(), now );
  qb.setColumnValue( PimItem::atimeColumn(), now );
  qb.setColumnValue( PimItem::dirtyColumn(), true );
  if ( !qb.exec() ) {
    return false;
  }

  transaction.commit();

  Q_FOREACH ( const Collection &col, collection.children() ) {
    if ( !recursiveSetResourceId( col, resourceId ) ) {
      return false;
    }
  }
  return true;
}

bool DataStore::moveCollection( Collection &collection, const Collection &newParent )
{
  if ( collection.parentId() == newParent.id() ) {
    return true;
  }

  if ( !m_dbOpened || !newParent.isValid() ) {
    return false;
  }

  const QByteArray oldResource = collection.resource().name().toLatin1();

  int resourceId = collection.resourceId();
  const Collection source = collection.parent();
  if ( newParent.id() > 0 ) { // not root
    resourceId = newParent.resourceId();
  }
  if ( !CollectionQueryHelper::canBeMovedTo( collection, newParent ) ) {
    return false;
  }

  collection.setParentId( newParent.id() );
  if ( collection.resourceId() != resourceId ) {
    collection.setResourceId( resourceId );
    collection.setRemoteId( QString() );
    collection.setRemoteRevision( QString() );
    if ( !recursiveSetResourceId( collection, resourceId ) ) {
      return false;
    }
  }

  if ( !collection.update() ) {
    return false;
  }

  mNotificationCollector->collectionMoved( collection, source, oldResource, newParent.resource().name().toLatin1() );
  return true;
}

bool DataStore::appendMimeTypeForCollection( qint64 collectionId, const QStringList &mimeTypes )
{
  if ( mimeTypes.isEmpty() ) {
    return true;
  }
  SelectQueryBuilder<MimeType> qb;
  qb.addValueCondition( MimeType::nameColumn(), Query::In, mimeTypes );
  if ( !qb.exec() ) {
    return false;
  }
  QStringList missingMimeTypes = mimeTypes;

  Q_FOREACH ( const MimeType &mt, qb.result() ) {
    // unique index on n:m relation prevents duplicates, ie. this will fail
    // if this mimetype is already set
    if ( !Collection::addMimeType( collectionId, mt.id() ) ) {
      return false;
    }
    missingMimeTypes.removeAll( mt.name() );
  }

  // the MIME type doesn't exist, so we have to add it to the db
  Q_FOREACH ( const QString &mtName, missingMimeTypes ) {
    qint64 mimeTypeId;
    if ( !appendMimeType( mtName, &mimeTypeId ) ) {
      return false;
    }
    if ( !Collection::addMimeType( collectionId, mimeTypeId ) ) {
      return false;
    }
  }

  return true;
}

void DataStore::activeCachePolicy( Collection &col )
{
  if ( !col.cachePolicyInherit() ) {
    return;
  }

  Collection parent = col;
  while ( parent.parentId() != 0 ) {
    parent = parent.parent();
    if ( !parent.cachePolicyInherit() ) {
      col.setCachePolicyCheckInterval( parent.cachePolicyCheckInterval() );
      col.setCachePolicyCacheTimeout( parent.cachePolicyCacheTimeout() );
      col.setCachePolicySyncOnDemand( parent.cachePolicySyncOnDemand() );
      col.setCachePolicyLocalParts( parent.cachePolicyLocalParts() );
      return;
    }
  }

  // ### system default
  col.setCachePolicyCheckInterval( -1 );
  col.setCachePolicyCacheTimeout( -1 );
  col.setCachePolicySyncOnDemand( false );
  col.setCachePolicyLocalParts( QLatin1String( "ALL" ) );
}

QVector<Collection> DataStore::virtualCollections( const PimItem &item )
{
  QueryBuilder qb( CollectionPimItemRelation::tableName(), QueryBuilder::Select );
  qb.addJoin( QueryBuilder::InnerJoin, Collection::tableName(),
              Collection::idFullColumnName(), CollectionPimItemRelation::leftFullColumnName() );
  Q_FOREACH ( const QString &columnName, Collection::fullColumnNames() ) {
    qb.addColumn( columnName );
  }
  qb.addValueCondition( CollectionPimItemRelation::rightFullColumnName(), Query::Equals, item.id() );

  if ( !qb.exec() ) {
    akDebug() << "Error during selection of records from table CollectionPimItemRelation"
              << qb.query().lastError().text();
    return QVector<Collection>();
  }

  return Collection::extractResult( qb.query() );
}

QMap<Entity::Id,PimItem> DataStore::virtualCollections( const PimItem::List &items )
{
  QueryBuilder qb( CollectionPimItemRelation::tableName(), QueryBuilder::Select );
  qb.addJoin( QueryBuilder::InnerJoin, Collection::tableName(),
              Collection::idFullColumnName(), CollectionPimItemRelation::leftFullColumnName() );
  qb.addJoin( QueryBuilder::InnerJoin, PimItem::tableName(),
              PimItem::idFullColumnName(), CollectionPimItemRelation::rightFullColumnName() );
  qb.addColumn( Collection::idFullColumnName() );
  qb.addColumns( QStringList() << PimItem::idFullColumnName()
                               << PimItem::remoteIdFullColumnName()
                               << PimItem::remoteRevisionFullColumnName() );

  Query::Condition condition( Query::Or );
  QStringList ids;
  Q_FOREACH ( const PimItem &item, items ) {
    ids << QString::number( item.id() );
  }
  qb.addValueCondition( CollectionPimItemRelation::rightFullColumnName(), Query::In, ids );

  if ( !qb.exec() ) {
    akDebug() << "Error during selection of records from table CollectionPimItemRelation"
              << qb.query().lastError().text();
    return QMap<Entity::Id,PimItem>();
  }

  QSqlQuery query = qb.query();
  QMap<Entity::Id,PimItem> map;
  while ( query.next() ) {
    PimItem item;
    item.setId( query.value( 1 ).toLongLong() );
    item.setRemoteId( query.value( 2 ).toString() );
    item.setRemoteRevision( query.value( 3 ).toString() );
    map.insertMulti( query.value( 0 ).toLongLong(), item );
  }

  return map;
}

/* --- MimeType ------------------------------------------------------ */
bool DataStore::appendMimeType( const QString &mimetype, qint64 *insertId )
{
  if ( MimeType::exists( mimetype ) ) {
    akDebug() << "Cannot insert mimetype " << mimetype
              << " because it already exists.";
    return false;
  }

  MimeType mt( mimetype );
  return mt.insert( insertId );
}

/* --- PimItem ------------------------------------------------------- */
bool DataStore::appendPimItem( QVector<Part> &parts,
                               const MimeType &mimetype,
                               const Collection &collection,
                               const QDateTime &dateTime,
                               const QString &remote_id,
                               const QString &remoteRevision,
                               const QString &gid,
                               PimItem &pimItem )
{
  pimItem.setMimeTypeId( mimetype.id() );
  pimItem.setCollectionId( collection.id() );
  if ( dateTime.isValid() ) {
    pimItem.setDatetime( dateTime );
  }
  if ( remote_id.isEmpty() ) {
    // from application
    pimItem.setDirty( true );
  } else {
    // from resource
    pimItem.setRemoteId( remote_id );
    pimItem.setDirty( false );
  }
  pimItem.setRemoteRevision( remoteRevision );
  pimItem.setGid( gid );
  pimItem.setAtime( QDateTime::currentDateTime() );

  if ( !pimItem.insert() ) {
    return false;
  }

  // insert every part
  if ( !parts.isEmpty() ) {
    //don't use foreach, the caller depends on knowing the part has changed, see the Append handler
    for ( QVector<Part>::iterator it = parts.begin(); it != parts.end(); ++it ) {

      ( *it ).setPimItemId( pimItem.id() );
      if ( ( *it ).datasize() < ( *it ).data().size() ) {
        ( *it ).setDatasize( ( *it ).data().size() );
      }

//      akDebug() << "Insert from DataStore::appendPimItem";
      if ( !PartHelper::insert( &( *it ) ) ) {
        return false;
      }
    }
  }

//   akDebug() << "appendPimItem: " << pimItem;

  mNotificationCollector->itemAdded( pimItem, collection );
  return true;
}

bool DataStore::unhidePimItem( PimItem &pimItem )
{
  if ( !m_dbOpened ) {
    return false;
  }

  akDebug() << "DataStore::unhidePimItem(" << pimItem << ")";

  // FIXME: This is inefficient. Using a bit on the PimItemTable record would probably be some orders of magnitude faster...
  QList< QByteArray > parts;
  parts << AKONADI_ATTRIBUTE_HIDDEN;

  return removeItemParts( pimItem, parts );
}

bool DataStore::unhideAllPimItems()
{
  if ( !m_dbOpened ) {
    return false;
  }

  akDebug() << "DataStore::unhideAllPimItems()";

  try {
    return PartHelper::remove( Part::partTypeIdFullColumnName(), PartTypeHelper::fromName( "ATR", "HIDDEN" ).id() );
  } catch ( ... ) {} // we can live with this failing

  return false;
}

bool DataStore::cleanupPimItems( const PimItem::List &items )
{
  // generate the notification before actually removing the data
  mNotificationCollector->itemsRemoved( items );

  // FIXME: Create a single query to do this
  Q_FOREACH ( const PimItem &item, items ) {
    if ( !item.clearFlags() ) {
      return false;
    }
    if ( !PartHelper::remove( Part::pimItemIdColumn(), item.id() ) ) {
      return false;
    }
    if ( !PimItem::remove( PimItem::idColumn(), item.id() ) ) {
      return false;
    }

    if ( !Entity::clearRelation<CollectionPimItemRelation>( item.id(), Entity::Right ) ) {
      return false;
    }
  }

  return true;
}

bool DataStore::addCollectionAttribute( const Collection &col, const QByteArray &key, const QByteArray &value )
{
  SelectQueryBuilder<CollectionAttribute> qb;
  qb.addValueCondition( CollectionAttribute::collectionIdColumn(), Query::Equals, col.id() );
  qb.addValueCondition( CollectionAttribute::typeColumn(), Query::Equals, key );
  if ( !qb.exec() ) {
    return false;
  }

  if ( qb.result().count() > 0 ) {
    akDebug() << "Attribute" << key << "already exists for collection" << col.id();
    return false;
  }

  CollectionAttribute attr;
  attr.setCollectionId( col.id() );
  attr.setType( key );
  attr.setValue( value );

  if ( !attr.insert() ) {
    return false;
  }

  mNotificationCollector->collectionChanged( col, QList<QByteArray>() << key );
  return true;
}

bool DataStore::removeCollectionAttribute( const Collection &col, const QByteArray &key )
{
  SelectQueryBuilder<CollectionAttribute> qb;
  qb.addValueCondition( CollectionAttribute::collectionIdColumn(), Query::Equals, col.id() );
  qb.addValueCondition( CollectionAttribute::typeColumn(), Query::Equals, key );
  if ( !qb.exec() ) {
    throw HandlerException( "Unable to query for collection attribute" );
  }

  const QVector<CollectionAttribute> result = qb.result();
  Q_FOREACH ( CollectionAttribute attr, result ) {
    if ( !attr.remove() ) {
      throw HandlerException( "Unable to remove collection attribute" );
    }
  }

  if ( !result.isEmpty() ) {
    mNotificationCollector->collectionChanged( col, QList<QByteArray>() << key );
    return true;
  }
  return false;
}

void DataStore::debugLastDbError( const char *actionDescription ) const
{
  akError() << "Database error:" << actionDescription;
  akError() << "  Last driver error:" << m_database.lastError().driverText();
  akError() << "  Last database error:" << m_database.lastError().databaseText();

  Tracer::self()->error( "DataStore (Database Error)",
                         QString::fromLatin1( "%1\nDriver said: %2\nDatabase said:%3" )
                            .arg( QString::fromLatin1( actionDescription ) )
                            .arg( m_database.lastError().driverText() )
                            .arg( m_database.lastError().databaseText() ) );
}

void DataStore::debugLastQueryError( const QSqlQuery &query, const char *actionDescription ) const
{
  akError() << "Query error:" << actionDescription;
  akError() << "  Last error message:" << query.lastError().text();
  akError() << "  Last driver error:" << m_database.lastError().driverText();
  akError() << "  Last database error:" << m_database.lastError().databaseText();

  Tracer::self()->error( "DataStore (Database Query Error)",
                         QString::fromLatin1( "%1: %2" )
                            .arg( QString::fromLatin1( actionDescription ) )
                            .arg( query.lastError().text() ) );
}

// static
QString DataStore::dateTimeFromQDateTime( const QDateTime &dateTime )
{
    QDateTime utcDateTime = dateTime;
    if ( utcDateTime.timeSpec() != Qt::UTC ) {
        utcDateTime.toUTC();
    }
    return utcDateTime.toString( QLatin1String( "yyyy-MM-dd hh:mm:ss" ) );
}

// static
QDateTime DataStore::dateTimeToQDateTime( const QByteArray &dateTime )
{
    return QDateTime::fromString( QString::fromLatin1( dateTime ), QLatin1String( "yyyy-MM-dd hh:mm:ss" ) );
}

void DataStore::addQueryToTransaction( const QSqlQuery &query, bool isBatch )
{
  DbType::Type dbType = DbType::type( m_database );
  // This is used for replaying deadlocked transactions, so only record queries
  // for backends that support concurrent transactions.
  if ( !inTransaction() || ( dbType != DbType::MySQL && dbType != DbType::PostgreSQL ) ) {
    return;
  }

  m_transactionQueries.append( qMakePair( query, isBatch ) );
}

QSqlQuery DataStore::retryLastTransaction()
{
  DbType::Type dbType = DbType::type( m_database );
  if ( !inTransaction() || ( dbType != DbType::MySQL && dbType != DbType::PostgreSQL ) ) {
    return QSqlQuery();
  }

  // The database has rolled back the actual transaction, so reset the counter
  // to 0 and start a new one in beginTransaction(). Then restore the level
  // because this has to be completely transparent to the original caller
  const int oldTransactionLevel = m_transactionLevel;
  m_transactionLevel = 0;
  if ( !beginTransaction() ) {
    m_transactionLevel = oldTransactionLevel;
    return QSqlQuery();
  }
  m_transactionLevel = oldTransactionLevel;

  QSqlQuery ret;
  typedef QPair<QSqlQuery, bool> QueryBoolPair;
  QMutableVectorIterator<QueryBoolPair> iter( m_transactionQueries );
  while ( iter.hasNext() ) {
    iter.next();
    QSqlQuery query = iter.value().first;
    const bool isBatch = iter.value().second;

    // Make sure the query is ready to be executed again
    if ( query.isActive() ) {
      query.finish();
    }

    bool res = false;
    if ( isBatch ) {
      // QSqlQuery::execBatch() does not reset lastError(), so for the sake
      // of transparency (make it look to the caller like if the query was
      // successful the first time), we create a copy of the original query,
      // which has lastError empty.
      QSqlQuery copiedQuery( m_database );
      copiedQuery.prepare( query.executedQuery() );
      const QVariantList values = query.boundValues().values();
      for (int i = 0; i < values.size(); ++i) {
        copiedQuery.bindValue(i, values[i]);
      }
      query = copiedQuery;
      res = query.execBatch();
    } else {
      res = query.exec();
    }

    if ( !res ) {
      // Don't do another deadlock detection here, just give up.
      akError() << "DATABASE ERROR:";
      akError() << "  Error code:" << query.lastError().number();
      akError() << "  DB error: " << query.lastError().databaseText();
      akError() << "  Error text:" << query.lastError().text();
      akError() << "  Query:" << query.executedQuery();

      // Return the last query, because that's what caller expects to retrieve
      // from QueryBuilder. It is in error state anyway.
      return m_transactionQueries.last().first;
    }

    // Update the query in the list
    iter.setValue( qMakePair( query, isBatch ) );
  }

  return m_transactionQueries.last().first;
}

bool DataStore::beginTransaction()
{
  if ( !m_dbOpened ) {
    return false;
  }

  if ( m_transactionLevel == 0 ) {
    TRANSACTION_MUTEX_LOCK;
    QSqlDriver *driver = m_database.driver();
    if ( !driver->beginTransaction() ) {
      debugLastDbError( "DataStore::beginTransaction" );
      TRANSACTION_MUTEX_UNLOCK;
      return false;
    }
  }

  ++m_transactionLevel;

  return true;
}

bool DataStore::rollbackTransaction()
{
  if ( !m_dbOpened ) {
    return false;
  }

  if ( m_transactionLevel == 0 ) {
    qWarning() << "DataStore::rollbackTransaction(): No transaction in progress!";
    return false;
  }

  --m_transactionLevel;

  if ( m_transactionLevel == 0 ) {
    QSqlDriver *driver = m_database.driver();
    Q_EMIT transactionRolledBack();
    if ( !driver->rollbackTransaction() ) {
      TRANSACTION_MUTEX_UNLOCK;
      debugLastDbError( "DataStore::rollbackTransaction" );
      return false;
    }
    TRANSACTION_MUTEX_UNLOCK;

    m_transactionQueries.clear();
  }

  return true;
}

bool DataStore::commitTransaction()
{
  if ( !m_dbOpened ) {
    return false;
  }

  if ( m_transactionLevel == 0 ) {
    qWarning() << "DataStore::commitTransaction(): No transaction in progress!";
    return false;
  }

  if ( m_transactionLevel == 1 ) {
    QSqlDriver *driver = m_database.driver();
    if ( !driver->commitTransaction() ) {
      debugLastDbError( "DataStore::commitTransaction" );
      rollbackTransaction();
      return false;
    } else {
      TRANSACTION_MUTEX_UNLOCK;
      Q_EMIT transactionCommitted();
    }

    m_transactionQueries.clear();
  }

  m_transactionLevel--;
  return true;
}

bool DataStore::inTransaction() const
{
  return m_transactionLevel > 0;
}

void DataStore::sendKeepAliveQuery()
{
  if ( m_database.isOpen() ) {
    QSqlQuery query( m_database );
    query.exec( QLatin1String( "SELECT 1" ) );
  }
}
