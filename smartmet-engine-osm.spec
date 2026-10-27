%define DIRNAME osm
%define LIBNAME smartmet-%{DIRNAME}
%define SPECNAME smartmet-engine-%{DIRNAME}
Summary: SmartMet OSM engine
Name: %{SPECNAME}
Version: 26.10.27
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

%define smartmet_fmt_min 12.0.0
%define smartmet_fmt_max 13.0.0
%define smartmet_fmt fmt-libs >= %{smartmet_fmt_min}, fmt-libs < %{smartmet_fmt_max}
%define smartmet_fmt_devel fmt-devel >= %{smartmet_fmt_min}, fmt-devel < %{smartmet_fmt_max}

BuildRequires: rpm-build
BuildRequires: gcc-c++
BuildRequires: make
BuildRequires: %{smartmet_boost}-devel
BuildRequires: smartmet-library-spine-devel >= 26.10.16
BuildRequires: smartmet-library-macgyver-devel >= 26.10.16
BuildRequires: smartmet-library-gis-devel >= 26.10.17
BuildRequires: smartmet-utils-devel >= 26.4.2
BuildRequires: %{smartmet_fmt_devel}
BuildRequires: gdal312-devel
BuildRequires: jsoncpp-devel
BuildRequires: libconfig17-devel
BuildRequires: zlib-devel
BuildRequires: libzstd-devel
%if 0%{?rhel} && 0%{rhel} == 8
BuildRequires: libpqxx-devel >= 1:7.7.0, libpqxx-devel < 1:7.8.0
Requires: libpqxx >= 1:7.7.0, libpqxx < 1:7.8.0
%else
%if 0%{?rhel} && 0%{rhel} == 9
BuildRequires: libpqxx-devel >= 1:7.9.0, libpqxx-devel < 1:7.10.0
Requires: libpqxx >= 1:7.9.0, libpqxx < 1:7.10.0
%else
%if 0%{?rhel} && 0%{rhel} >= 10
BuildRequires: libpqxx-devel >= 1:7.10.0, libpqxx-devel < 1:7.11.0
Requires: libpqxx >= 1:7.10.0, libpqxx < 1:7.11.0
%else
BuildRequires: libpqxx-devel
Requires: libpqxx
%endif
%endif
%endif
Requires: %{smartmet_boost}-thread
Requires: smartmet-library-spine >= 26.10.17
Requires: smartmet-library-macgyver >= 26.10.17
Requires: smartmet-library-gis >= 26.10.17
Requires: %{smartmet_fmt}
Requires: gdal312-libs
Requires: jsoncpp
Requires: libconfig17
Requires: zlib
Requires: libzstd
# smartmet-osm-pgload
Requires: %{smartmet_boost}-program-options
# smartmet-osm-update
Requires: curl
Requires: python3
Requires: util-linux
Provides: %{SPECNAME}

%description
FMI SmartMet OSM engine serving vector tile datasets such as OpenStreetMap
basemaps from PMTiles v3 files in a configured data directory or from a
PostgreSQL database, and rendering them server side with MapLibre styles.
The data itself is not packaged.

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
%{_bindir}/smartmet-osm-update
%{_bindir}/smartmet-osm-pgload
%defattr(0664,root,root,0775)
%{_datadir}/smartmet/osm/styles/*.json

%files -n %{SPECNAME}-devel
%defattr(0664,root,root,0775)
%{_includedir}/smartmet/engines/%{DIRNAME}/*.h

%changelog
* Tue Oct 27 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.10.27-1.fmi
- Fixed smartmet-osm-pgload build on RHEL9: exec(query, params) requires libpqxx 7.10, older versions use exec_params

* Mon Oct 26 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.10.26-1.fmi
- Rendering, style expressions, colour parsing, case conversion, tile store scans, configuration and overzooming split into smaller functions
- No use of a moved-from vector when splitting projected lines

* Sat Oct 24 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.10.24-2.fmi
- The coastline-lakes styles draw lake shores only, not the banks of river and canal areas

* Sat Oct 24 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.10.24-1.fmi
- New coastline-* styles: transparent overlays of the sea coastline, optionally with lake shores

* Fri Oct 23 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.10.23-2.fmi
- Fixed linking smartmet-osm-pgload on RHEL8: Boost library path and libpthread

* Fri Oct 23 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.10.23-1.fmi
- Datasets: the PMTiles files found in the configured directory (one dataset per
  subdirectory) are combined by resolution, each tile coming from the most detailed
  file and missing zoom levels from ancestor tiles; the directories are rescanned
  periodically
- Server side rendering of the datasets with MapLibre styles into SVG elements
  (fill, line and point symbol layers, expressions and legacy filters, any CRS)
- Vector tiles beyond the available zoom levels are cut from their ancestors
- Ships the Protomaps basemap styles, VersaTiles themes for the Protomaps schema
  and new meteorological background styles
- smartmet-osm-update downloads and installs the Protomaps basemap builds (the
  whole planet or extracts) atomically
- The engine interface is virtual so that plugins need not link against it
- Fast rendering: per-tile interpolated projection grids, direct coordinate formatting
- Fixed leaf directory lookups, which missed most tiles of large files
- Tiles are decompressed according to the tile compression of the file
- The leaf directory cache is now bounded by leaf_cache_size
- Files on network file systems are read with pread instead of mmap
  (file_access = auto/mmap/pread), and a read failure such as ESTALE from a
  file replaced on an NFS server makes the engine rescan and retry once
- Of files whose names differ only by a version suffix (planet-20261017.pmtiles)
  only the newest is used, so versions can be installed side by side;
  smartmet-osm-update -V installs builds under versioned names and deletes
  the previous ones after a grace period (-k days)
- Datasets can be stored in PostgreSQL (postgresql setting): versions are
  loaded and published with the new smartmet-osm-pgload tool, publishing is
  atomic, and the tiles of a map are fetched with a single query

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
