Name: libgnfcdc

Version: 1.1.1
Release: 0
Summary: Glib based NFC Daemon Client
Group: Development/Libraries
License: BSD
URL: https://github.com/monich/libgnfcdc
Source: %{name}-%{version}.tar.bz2

%define libglibutil_version 1.0.71
%define glib_version 2.32

BuildRequires: pkgconfig
BuildRequires: pkgconfig(glib-2.0) >= %{glib_version}
BuildRequires: pkgconfig(libglibutil) >= %{libglibutil_version}

# license macro requires rpm >= 4.11
BuildRequires: pkgconfig(rpm)
%define license_support %(pkg-config --exists 'rpm >= 4.11'; echo $?)

# make_build macro appeared in rpm 4.12
%{!?make_build:%define make_build make %{_smp_mflags}}

Requires: glib2 >= %{glib_version}
Requires: libglibutil >= %{libglibutil_version}
Requires(post): /sbin/ldconfig
Requires(postun): /sbin/ldconfig

%description
Glib based API for nfcd D-Bus interface.

%package devel
Summary: Development library for %{name}
Requires: %{name} = %{version}

%description devel
This package contains the development library for %{name}.

%prep
%setup -q

%build
%make_build LIBDIR=%{_libdir} KEEP_SYMBOLS=1 release pkgconfig

%install
rm -rf %{buildroot}
make LIBDIR=%{_libdir} DESTDIR=%{buildroot} install-dev

%post -p /sbin/ldconfig

%postun -p /sbin/ldconfig

%files
%defattr(-,root,root,-)
%{_libdir}/%{name}.so.*
%if %{license_support} == 0
%license LICENSE
%endif

%files devel
%defattr(-,root,root,-)
%{_libdir}/pkgconfig/*.pc
%{_libdir}/%{name}.so
%{_includedir}/gnfcdc/*.h
