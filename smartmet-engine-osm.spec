%define DIRNAME osm
%define LIBNAME smartmet-%{DIRNAME}
%define SPECNAME smartmet-engine-%{DIRNAME}
Summary: SmartMet OSM engine
Name: %{SPECNAME}
Version: 26.4.2
Release: 1%{?dist}.fmi
License: MIT
Group: SmartMet/Engines
URL: https://github.com/fmidev/smartmet-engine-osm
Source0: %{name}.tar.gz
BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-root-%(%{__id_u} -n)

# https://fedoraproject.org/wiki/Changes/Broken_RPATH_will_fail_rpmbuild
%global __brp_check_rpaths %{nil}

%if 0%{?rhel} && 0%{rhel} < 9
%define smartmet_boost boost169
%else
%define smartmet_boost boost
%endif

BuildRequires: rpm-build
BuildRequires: gcc-c++
BuildRequires: make
BuildRequires: %{smartmet_boost}-devel
BuildRequires: smartmet-library-spine-devel >= 26.4.2
BuildRequires: smartmet-library-macgyver-devel >= 26.4.2
BuildRequires: smartmet-utils-devel >= 26.4.2
BuildRequires: libconfig-devel
BuildRequires: zlib-devel
BuildRequires: libzstd-devel
Requires: %{smartmet_boost}-thread
Requires: smartmet-library-spine >= 26.4.2
Requires: smartmet-library-macgyver >= 26.4.2
Requires: libconfig
Requires: zlib
Requires: libzstd
Provides: %{SPECNAME}

%description
FMI SmartMet OSM engine providing memory-mapped access to PMTiles v3 files
containing pre-tiled OpenStreetMap vector data.

%package -n %{SPECNAME}-devel
Summary: SmartMet %{SPECNAME} development headers
Group: SmartMet/Development
Provides: %{SPECNAME}-devel
Requires: %{SPECNAME} = %{version}-%{release}
Requires: smartmet-library-spine-devel >= 26.4.2
Requires: smartmet-library-macgyver-devel >= 26.4.2
%description -n %{SPECNAME}-devel
SmartMet %{SPECNAME} development headers.

%prep
rm -rf $RPM_BUILD_ROOT

%setup -q -n %{SPECNAME}

%build -q -n %{SPECNAME}
make %{_smp_mflags}

%install
%makeinstall

%clean
rm -rf $RPM_BUILD_ROOT

%files -n %{SPECNAME}
%defattr(0775,root,root,0775)
%{_datadir}/smartmet/engines/%{DIRNAME}.so

%files -n %{SPECNAME}-devel
%defattr(0664,root,root,0775)
%{_includedir}/smartmet/engines/%{DIRNAME}/*.h

%changelog
* Thu Apr  2 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> 26.4.2-1.fmi
- Initial release: PMTiles-backed OSM engine
