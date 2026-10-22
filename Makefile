SUBNAME = osm
SPEC = smartmet-engine-$(SUBNAME)
INCDIR = smartmet/engines/$(SUBNAME)

REQUIRES = configpp fmt gdal jsoncpp libpqxx

include $(shell echo $${PREFIX-/usr})/share/smartmet/devel/makefile.inc

DEFINES = -DUNIX -D_REENTRANT

LIBS += $(PREFIX_LDFLAGS) \
	$(REQUIRED_LIBS) \
	-lsmartmet-spine \
	-lsmartmet-gis \
	-lsmartmet-macgyver \
	-lboost_thread \
	-lconfig++ \
	-lzstd \
	-lz \
	-lpthread

# What to install

LIBFILE = $(SUBNAME).so

# Compilation directories

vpath %.cpp $(SUBNAME)
vpath %.h $(SUBNAME)

# The files to be compiled

SRCS = $(wildcard $(SUBNAME)/*.cpp)
HDRS = $(wildcard $(SUBNAME)/*.h)
OBJS = $(patsubst %.cpp, obj/%.o, $(notdir $(SRCS)))

INCLUDES := -Iinclude $(INCLUDES)

.PHONY: test rpm

# The rules

PGLOAD = tools/smartmet-osm-pgload

all: objdir $(LIBFILE) $(PGLOAD)
debug: all
release: all
profile: all

$(LIBFILE): $(OBJS)
	$(CXX) $(LDFLAGS) -shared -rdynamic -o $(LIBFILE) $(OBJS) $(LIBS)
	@echo Checking $(LIBFILE) for unresolved references
	@if ldd -r $(LIBFILE) 2>&1 | c++filt | grep ^undefined\ symbol |\
			grep -Pv ':\ __(?:(?:a|t|ub)san_|sanitizer_)'; \
	then \
		rm -v $(LIBFILE); \
		exit 1; \
	fi

# Loader of PMTiles files into PostgreSQL, uses the reader of the engine
$(PGLOAD): tools/pgload.cpp obj/PMTilesReader.o obj/TileSource.o
	$(CXX) $(CFLAGS) $(INCLUDES) -o $@ tools/pgload.cpp obj/PMTilesReader.o obj/TileSource.o \
		$(PREFIX_LDFLAGS) $(REQUIRED_LIBS) -lsmartmet-macgyver -lboost_program_options -lzstd -lz

clean:
	rm -f $(LIBFILE) $(PGLOAD) obj/* *~ $(SUBNAME)/*~
	if test -f test/Makefile ; then $(MAKE) -C test $@; fi

format:
	clang-format -i -style=file $(SUBNAME)/*.h $(SUBNAME)/*.cpp

install:
	@mkdir -p $(includedir)/$(INCDIR)
	@list='$(HDRS)'; \
	for hdr in $$list; do \
	  HDR=$$(basename $$hdr); \
	  echo $(INSTALL_DATA) $$hdr $(includedir)/$(INCDIR)/$$HDR; \
	  $(INSTALL_DATA) $$hdr $(includedir)/$(INCDIR)/$$HDR; \
	done
	@mkdir -p $(enginedir)
	$(INSTALL_PROG) $(LIBFILE) $(enginedir)/$(LIBFILE)
	@mkdir -p $(bindir)
	$(INSTALL_PROG) tools/smartmet-osm-update $(bindir)/smartmet-osm-update
	$(INSTALL_PROG) $(PGLOAD) $(bindir)/smartmet-osm-pgload
	@mkdir -p $(datadir)/smartmet/osm/styles
	@for style in styles/protomaps-*.json styles/versatiles-*.json styles/meteo-*.json; do \
	  echo $(INSTALL_DATA) $$style $(datadir)/smartmet/osm/styles/; \
	  $(INSTALL_DATA) $$style $(datadir)/smartmet/osm/styles/; \
	done

test:
	if test -f test/Makefile ; then $(MAKE) -C test $@; fi

objdir:
	@mkdir -p $(objdir)

rpm: clean $(SPEC).spec
	rm -f $(SPEC).tar.gz
	tar -czvf $(SPEC).tar.gz --exclude test --exclude node_modules --exclude-vcs --transform "s,^,$(SPEC)/," *
	rpmbuild -tb $(SPEC).tar.gz
	rm -f $(SPEC).tar.gz

.SUFFIXES: $(SUFFIXES) .cpp

obj/%.o: %.cpp
	$(CXX) $(CFLAGS) $(INCLUDES) -c -MD -MF $(patsubst obj/%.o, obj/%.d, $@) -MT $@ -o $@ $<

ifneq ($(wildcard obj/*.d),)
-include $(wildcard obj/*.d)
endif
