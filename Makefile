# tblog build. `make` -> build/libtblog.a, `make test`, `make SAN=thread test`,
# `make STD=c++11 test` (C++11/14/17),
# `make SAN=address test`, `make bench`.
CXX      ?= g++
CC       ?= gcc
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
# -lz; programs linking libtblog.a then need -lz too). Without it the library
# spawns `gzip -n`. Override with `make HAVE_ZLIB=0`.
ifeq ($(origin HAVE_ZLIB),undefined)
  HAVE_ZLIB := $(shell printf '\043include <zlib.h>\nint main(void){return zlibVersion()==0;}\n' | \
                 $(CC) -x c - -lz -o /dev/null >/dev/null 2>&1 && echo 1 || echo 0)
endif
ifeq ($(HAVE_ZLIB),1)
  CXXFLAGS += -DTBLOG_HAVE_ZLIB=1
  LDFLAGS  += -lz
endif
ifneq ($(SAN),)
  CXXFLAGS += -fsanitize=$(SAN)
  CFLAGS   += -fsanitize=$(SAN)
  LDFLAGS  += -fsanitize=$(SAN)
endif

SRCS  := $(shell find src -name '*.cpp')
OBJS  := $(patsubst src/%.cpp,$(BUILD)/obj/%.o,$(SRCS))
LIB   := $(BUILD)/libtblog.a
TESTS := $(patsubst tests/unit/%.cpp,$(BUILD)/tests/%,$(wildcard tests/unit/*.cpp))

.PHONY: all test bench examples clean
all: $(LIB)

$(BUILD)/obj/%.o: src/%.cpp src/internal.h include/tblog/log.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(LIB): $(OBJS)
	@mkdir -p $(dir $@)
	ar rcs $@ $^

$(BUILD)/tests/%: tests/unit/%.cpp $(LIB) tests/tiny_test.h
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

examples: $(patsubst examples/%.c,$(BUILD)/examples/%,$(wildcard examples/*.c))

clean:
	rm -rf build build-*
