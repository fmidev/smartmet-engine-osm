PROG = $(patsubst %.cpp,%,$(wildcard *Test.cpp))

REQUIRES = configpp

include $(shell echo $${PREFIX-/usr})/share/smartmet/devel/makefile.inc

CFLAGS = -DUNIX -O0 -g $(FLAGS)

INCLUDES += \
	-I../osm \
	-isystem /usr/include/smartmet

LIBS += \
	../osm.so \
	$(REQUIRED_LIBS) \
	-lsmartmet-spine \
	-lsmartmet-macgyver \
	-lboost_thread \
	-lconfig++ \
	-lzstd \
	-lz \
	-lpthread

all: $(PROG)

clean:
	rm -f $(PROG) *~

test: $(PROG)
	@err=0; \
	for prog in $(PROG); do \
	  ./$$prog || err=1; \
	done; \
	exit $$err

$(PROG) : % : %.cpp Makefile ../osm.so
	$(CXX) $(CFLAGS) -o $@ $@.cpp $(INCLUDES) $(LIBS)

.PHONY: all clean test
