%define soprano_version 2.7

%define rel 1

Name: akonadi
Summary: An extensible cross-desktop storage service for PIM
Version: 1.11.0
Release: %mkrel %{rel}
Epoch: 1
Url: http://pim.kde.org/akonadi/
License: LGPLv2+
Group: Networking/WWW
Source0: http://fr2.rpmfind.net/linux/KDE/stable/akonadi/src/%{name}-%{version}.tar.bz2
BuildRequires: pkgconfig(QtCore) < 5.0.0
BuildRequires: shared-mime-info >=  0.20
BuildRequires: pkgconfig(soprano) >= %{soprano_version}
BuildRequires: pkgconfig(sqlite3)
BuildRequires: cmake
BuildRequires: libxslt-proc
BuildRequires: libxml2-utils
BuildRequires: automoc
BuildRequires: mysql-devel
BuildRequires: postgresql-devel-virtual
BuildRequires: boost-devel
Requires: qt4-database-plugin-mysql
Obsoletes: akonadi-common < 1:1.6.90
Requires: mysql-core
Requires: mysql-common
# Needed for mysqlcheck  which is used in akonadi
Requires: mysql-client

%description
An extensible cross-desktop storage service for PIM data and meta data
providing concurrent read, write, and query access.

%files
%doc README
%{_bindir}/*
%dir %{_sysconfdir}/akonadi
%config %{_sysconfdir}/akonadi/mysql-global-mobile.conf
%config %{_sysconfdir}/akonadi/mysql-global.conf
%dir %{_libdir}/%{name}
%{_datadir}/dbus-1/services/*
%{_datadir}/dbus-1/interfaces/*.xml
%{_datadir}/mime/packages/akonadi-mime.xml
%{_libdir}/plugins/sqldrivers/libqsqlite3.so

#------------------------------------------------------------------------------

%define akonadiprotocolinternals_major 1
%define libakonadiprotocolinternals %mklibname akonadiprotocolinternals %{akonadiprotocolinternals_major}

%package -n     %libakonadiprotocolinternals
Summary:        Akonadi runtime library
Group:          System/Libraries

%description -n %libakonadiprotocolinternals
%name library.

%files -n %libakonadiprotocolinternals
%_libdir/libakonadiprotocolinternals.so.%{akonadiprotocolinternals_major}*

#------------------------------------------------------------------------------

%define akonadi_devel %mklibname akonadi -d

%package   -n %akonadi_devel
Summary:   Devel stuff for %name
Group:     Development/KDE and Qt
Obsoletes:  akonadi-devel < 1:1.6.90
Requires:  akonadi = %epoch:%version
Requires:  %libakonadiprotocolinternals = %epoch:%version
Provides:  akonadi-devel = %epoch:%version-%release

%description  -n %akonadi_devel
This package contains header files needed if you wish to build applications
based on %name

%files -n %akonadi_devel
%{_includedir}/*
%{_libdir}/*.so
%{_libdir}/pkgconfig/akonadi.pc
%{_libdir}/cmake/Akonadi

#------------------------------------------------------------------------------

%prep
%setup -q

%build
%cmake_qt4 -DMYSQLD_EXECUTABLE=%_sbindir/mysqld -DCONFIG_INSTALL_DIR=%{_sysconfdir}
%make

%install
%makeinstall_std -C build

# We own this as this is present in some packages like korganiser
mkdir -p %buildroot%{_libdir}/%{name}
