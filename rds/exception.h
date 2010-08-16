/***************************************************************************
 *   Copyright (C) 2010 by Marc Mutz <mutz@kde.org>                        *
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
#ifndef __AKONADI_RDS_EXCEPTION_H__
#define __AKONADI_RDS_EXCEPTION_H__

#include <QString>
#include <stdexcept>

template <typename Ex>
class Exception : Ex {
public:
    explicit Exception( const QString & msg )
#ifdef QT_NO_STL
        : Ex( std::string( qPrintable(msg) ) ) {}
#else
        : Ex( msg.toStdString() ) {}
#endif
    ~Exception() throw() {}
};


#endif
