%define DIRNAME osm
%define LIBNAME smartmet-%{DIRNAME}
%define SPECNAME smartmet-engine-%{DIRNAME}
Summary: SmartMet OSM engine
Name: %{SPECNAME}
Version: 26.10.17
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
BuildRequires: smartmet-library-spine-devel >= 26.10.16
BuildRequires: smartmet-library-macgyver-devel >= 26.10.16
BuildRequires: smartmet-utils-devel >= 26.4.2
BuildRequires: libconfig17-devel
BuildRequires: zlib-devel
BuildRequires: libzstd-devel
Requires: %{smartmet_boost}-thread
Requires: smartmet-library-spine >= 26.10.17
Requires: smartmet-library-macgyver >= 26.10.17
Requires: libconfig17
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
Requires: smartmet-library-spine-devel >= 26.10.16
Requires: smartmet-library-macgyver-devel >= 26.10.16
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
* Sat Oct 17 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.10.17-1.fmi
- Rebuilt against the new library versions

* Fri Oct 16 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.10.16-1.fmi
- Fixed compiler warnings and clang-tidy findings (unused code, unreachable code, empty statements, member initialization, parameter passing, noexcept), no functional changes

* Thu Oct 15 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.10.15-1.fmi
- Fixed clang-tidy style findings (loops, nested conditionals, dead code)
- Requires 26.10.15 versions of spine due to their ABI changes
- Fixed clang-tidy correctness findings (member initialization, rule of five, printf formats, dead stores)

* Wed Sep 30 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> 26.9.30-1.fmi
- Use libconfig17 instead of libconfig17
* Fri Aug 29 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> 26.8.29-1.fmi
- Harden PMTiles reader against hostile files: overflow-safe bounds checks on
  all file-controlled offsets/lengths, capped directory-entry allocation, and a
  leaf-directory recursion depth limit

* Thu Apr  2 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> 26.4.2-1.fmi
- Initial release: PMTiles-backed OSM engine
