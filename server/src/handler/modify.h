/*
    Copyright (c) 2006 Volker Krause <volker.krause@rwth-aachen.de>

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

#ifndef AKONADI_MODIFY_H
#define AKONADI_MODIFY_H

#include <handler.h>

namespace Akonadi {

/**
  @ingroup akonadi_server_handler

  Handler for the MODIFY command (not in RFC 3501).

  This command is used to modify collections. Its syntax is similar to the STORE
  command.

  <h4>Syntax</h4>

  Request:
  @verbatim
  request = tag " MODIFY " collection-id " " attribute-list
  attribute-list = *([-]attribute-name [" " attribute-value])
  attribute-name = "NAME" | "MIMETYPE" | "REMOTEID" | "CACHEPOLICY" | "PARENT" | [-]custom-attr-name
  @endverbatim

  Attributes marked with a leading '-' will be deleted, they don't have any attribute value.
*/
class Modify : public Handler
{
  Q_OBJECT
  public:
    Modify();
    ~Modify();
    bool handleLine( const QByteArray &line );
};

}

#endif
