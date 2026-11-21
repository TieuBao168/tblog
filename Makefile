# tblog - Linux convenience build. CMake (CMakeLists.txt) is the primary,
# CI-verified build; this Makefile mirrors it for quick work on Linux.
# `make` -> build/libtblog.a, `make test`, `make SAN=thread test`,
# `make STD=c++11 test` (C++11/14/17),
# `make SAN=address test`, `make bench`.
# Packaging: `make shared` / `make static`, `make install PREFIX=/usr/local
# DESTDIR=`, `make uninstall`, `make check-symbols`, `make check-install`.
CXX      ?= g++
CC       ?= gcc
AR       ?= ar
SAN      ?=
# C++ standard used to build the library: c++11, c++14 or c++17 (default).
STD      ?= c++17
BUILD    := build$(if $(SAN),-$(SAN),)$(if $(filter-out c++17,$(STD)),-$(STD),)
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=$(STD) -Wall -Wextra -pthread -Iinclude -Isrc -fno-omit-frame-pointer
CFLAGS   ?= -O2 -g
CFLAGS   += -std=c99 -Wall -Wextra -pthread -Iinclude
LDFLAGS  += -pthread -rdynamic
# zlib for file.compress=gzip, autodetected (HAVE_ZLIB=1: needs <zlib.h> and
# -lz; programs linking libtblog.a then need -lz too). Without it
# file.compress = gzip is ignored (with a warning). Override with `make HAVE_ZLIB=0`.
ifeq ($(origin HAVE_ZLIB),undefined)
  HAVE_ZLIB := $(shell printf '\043include <zlib.h>\nint main(void){return zlibVersion()==0;}\n' | \
                 $(CC) -x c - -lz -o /dev/null >/dev/null 2>&1 && echo 1 || echo 0)
endif
# Libraries libtblog itself depends on: linked AFTER the objects of the .so
# (--as-needed would drop them otherwise) and listed in tblog.pc.
LIB_LIBS :=
ifeq ($(HAVE_ZLIB),1)
  CXXFLAGS += -DTBLOG_HAVE_ZLIB=1
  LIB_LIBS += -lz
  LDFLAGS  += -lz
endif
ifneq ($(SAN),)
  CXXFLAGS += -fsanitize=$(SAN)
  CFLAGS   += -fsanitize=$(SAN)
  LDFLAGS  += -fsanitize=$(SAN)
endif

# Portable sources plus the POSIX platform layer. Other platform directories
# (src/platform/win32, ...) are not built by this Linux-only Makefile.
# Source basenames must be unique: `ar` keys archive members by file name.
SRCS  := $(shell find src -name '*.cpp' -not -path 'src/platform/*') \
         $(wildcard src/platform/posix/*.cpp)
# Internal headers: any change rebuilds the library objects and the tests.
LIB_HDRS := $(shell find src -name '*.h' -not -path 'src/platform/*') \
            src/platform/platform.h $(wildcard src/platform/posix/*.h)
OBJS  := $(patsubst src/%.cpp,$(BUILD)/obj/%.o,$(SRCS))
LIB   := $(BUILD)/libtblog.a
TESTS := $(patsubst tests/unit/%.cpp,$(BUILD)/tests/%,$(wildcard tests/unit/*.cpp))

# Shared library: libtblog.so.$(VERSION), soname libtblog.so.$(SOVERSION).
VERSION   := 0.1.0
SOVERSION := 0
SONAME    := libtblog.so.$(SOVERSION)
SHLIB     := $(BUILD)/libtblog.so.$(VERSION)
SHLINKS   := $(BUILD)/$(SONAME) $(BUILD)/libtblog.so
# Library objects are PIC (shared by .a and .so) and hide everything that is
# not marked TBLOG_API; the version script also hides std:: instantiations.
LIB_CXXFLAGS := -fPIC -fvisibility=hidden
SHLIB_LDFLAGS = -shared -Wl,-soname,$(SONAME) -Wl,--version-script=packaging/tblog.map \
                $(if $(SAN),,-Wl,--no-undefined)

.PHONY: all static shared test bench examples clean install uninstall check-symbols check-install
all: $(LIB) $(SHLINKS)
static: $(LIB)
shared: $(SHLINKS)

$(BUILD)/obj/%.o: src/%.cpp $(LIB_HDRS) include/tblog/log.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(LIB_CXXFLAGS) -c $< -o $@

$(LIB): $(OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(SHLIB): $(OBJS) packaging/tblog.map
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(filter-out -rdynamic,$(LDFLAGS)) $(SHLIB_LDFLAGS) $(OBJS) $(LIB_LIBS) -o $@

$(BUILD)/$(SONAME): $(SHLIB)
	ln -sf $(notdir $<) $@

$(BUILD)/libtblog.so: $(BUILD)/$(SONAME)
	ln -sf $(notdir $<) $@

# Public headers are prerequisites so header-only changes rebuild the tests.
PUBLIC_HDRS := $(wildcard include/tblog/*.h include/tblog/*.hpp include/tblog/detail/*.hpp)

$(BUILD)/tests/%: tests/unit/%.cpp $(LIB) $(wildcard tests/*.h) $(PUBLIC_HDRS) $(LIB_HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -Itests $< $(LIB) $(LDFLAGS) -o $@

# TSan aborts with "unexpected memory mapping" under high-entropy ASLR on new
# kernels; run its binaries with ASLR disabled.
RUNNER := $(if $(filter thread,$(SAN)),setarch $(shell uname -m) -R,)

test: $(TESTS)
	@set -e; for t in $(TESTS); do echo "=== $$t"; TBLOG_TEST_DIR=$(BUILD)/tmp $(RUNNER) $$t; done

$(BUILD)/bench/%: bench/%.cpp $(LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $< $(LIB) $(LDFLAGS) -o $@

bench: $(patsubst bench/%.cpp,$(BUILD)/bench/%,$(wildcard bench/*.cpp))

$(BUILD)/examples/%: examples/%.c $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< $(LIB) $(LDFLAGS) -lstdc++ -o $@

$(BUILD)/examples/%: examples/%.cpp $(LIB) $(PUBLIC_HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $< $(LIB) $(LDFLAGS) -o $@

examples: $(patsubst examples/%.c,$(BUILD)/examples/%,$(wildcard examples/*.c)) \
          $(patsubst examples/%.cpp,$(BUILD)/examples/%,$(wildcard examples/*.cpp))

# ---- install -----------------------------------------------------------------
PREFIX       ?= /usr/local
DESTDIR      ?=
LIBDIR       ?= $(PREFIX)/lib
INCLUDEDIR   ?= $(PREFIX)/include
PKGCONFIGDIR ?= $(LIBDIR)/pkgconfig
INSTALL      ?= install
NM           ?= nm
PKG_CONFIG   ?= pkg-config
HEADERS      := $(wildcard include/tblog/*.h include/tblog/*.hpp)
# .pc paths are written relative to ${prefix} when they live under PREFIX.
PC_LIBDIR     = $(patsubst $(PREFIX)/%,$${prefix}/%,$(LIBDIR))
PC_INCLUDEDIR = $(patsubst $(PREFIX)/%,$${prefix}/%,$(INCLUDEDIR))

install: $(LIB) $(SHLINKS) packaging/tblog.pc.in
	$(INSTALL) -d $(DESTDIR)$(INCLUDEDIR)/tblog $(DESTDIR)$(LIBDIR) $(DESTDIR)$(PKGCONFIGDIR)
	$(INSTALL) -m 644 $(HEADERS) $(DESTDIR)$(INCLUDEDIR)/tblog/
	$(INSTALL) -m 644 $(LIB) $(DESTDIR)$(LIBDIR)/
	$(INSTALL) -m 755 $(SHLIB) $(DESTDIR)$(LIBDIR)/
	ln -sf libtblog.so.$(VERSION) $(DESTDIR)$(LIBDIR)/$(SONAME)
	ln -sf $(SONAME) $(DESTDIR)$(LIBDIR)/libtblog.so
	sed -e 's|@TBLOG_PC_PREFIX@|$(PREFIX)|' -e 's|@TBLOG_PC_LIBDIR@|$(PC_LIBDIR)|' \
	    -e 's|@TBLOG_PC_INCLUDEDIR@|$(PC_INCLUDEDIR)|' -e 's|@TBLOG_VERSION@|$(VERSION)|' \
	    -e 's|@TBLOG_PC_PRIVATE_LIBS@|$(strip $(LIB_LIBS))|' \
	    packaging/tblog.pc.in > $(DESTDIR)$(PKGCONFIGDIR)/tblog.pc
	chmod 644 $(DESTDIR)$(PKGCONFIGDIR)/tblog.pc

uninstall:
	rm -f $(addprefix $(DESTDIR)$(INCLUDEDIR)/tblog/,$(notdir $(HEADERS)))
	-rmdir $(DESTDIR)$(INCLUDEDIR)/tblog 2>/dev/null
	rm -f $(DESTDIR)$(LIBDIR)/libtblog.a $(DESTDIR)$(LIBDIR)/libtblog.so.$(VERSION) \
	      $(DESTDIR)$(LIBDIR)/$(SONAME) $(DESTDIR)$(LIBDIR)/libtblog.so \
	      $(DESTDIR)$(PKGCONFIGDIR)/tblog.pc

# The .so must export exactly the tblog_* functions that libtblog.a defines.
check-symbols: $(LIB) $(SHLINKS)
	NM="$(NM)" sh packaging/check-symbols.sh $(LIB) $(SHLIB)

# Install into $(BUILD)/check-install, then build and run examples/basic.c
# against the installed static and shared libs via pkg-config.
check-install: $(LIB) $(SHLINKS) check-symbols
	BUILD="$(BUILD)" MAKE="$(MAKE)" CC="$(CC)" CFLAGS="$(CFLAGS)" LDFLAGS="$(LDFLAGS)" \
	RUNNER="$(RUNNER)" PKG_CONFIG="$(PKG_CONFIG)" sh packaging/check-install.sh

clean:
	rm -rf build build-*
