# tblog-lite: C99 logging for microcontrollers. Host build and tests.
#   make            -> build/libtblog_lite.a (core + printf, host)
#   make test       -> printf (int + float), ring, ISR, strip, port syntax checks
#                      (fails if a tests/test_*.c is not part of TESTS)
#   make test-asan  -> same tests with -fsanitize=address,undefined
#   make size       -> footprint of -Os objects (host x86-64, and Cortex-M
#                      if arm-none-eabi-gcc is on PATH)
#   make demo       -> build/demo (host demo)
ifeq ($(origin CC),default)
CC      := gcc
endif
SAN     ?=
WFLAGS  := -std=c99 -Wall -Wextra -pedantic -Werror
CFLAGS  ?= -O2 -g
SANLIST := address,undefined
SANFLAGS := $(if $(SAN),-fsanitize=$(SANLIST) -fno-sanitize-recover=all -fno-omit-frame-pointer)
ALLFLAGS = $(WFLAGS) $(CFLAGS) $(SANFLAGS) -Iinclude -Iport/posix
B       := build$(if $(SAN),-san)
ARMCC   ?= arm-none-eabi-gcc

CORE    := src/tblog_lite.c src/tblog_lite_printf.c
HDRS    := include/tblog_lite.h include/tblog_lite_config.h
POSIX   := port/posix/tblog_lite_port_posix.c

.PHONY: all test test-asan size demo check-ports clean
all: $(B)/libtblog_lite.a

$(B)/obj/%.o: src/%.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(ALLFLAGS) -c $< -o $@

$(B)/libtblog_lite.a: $(B)/obj/tblog_lite.o $(B)/obj/tblog_lite_printf.o
	ar rcs $@ $^

$(B)/test_printf: tests/test_printf.c src/tblog_lite_printf.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(ALLFLAGS) -o $@ tests/test_printf.c src/tblog_lite_printf.c

$(B)/test_printf_float: tests/test_printf.c src/tblog_lite_printf.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(ALLFLAGS) -DTBLOG_LITE_FLOAT=1 -o $@ tests/test_printf.c src/tblog_lite_printf.c

$(B)/test_ring: tests/test_ring.c $(CORE) $(POSIX) $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(ALLFLAGS) -o $@ tests/test_ring.c $(CORE) $(POSIX) -pthread

# Small ring and line: many more wraps/drops per byte logged.
$(B)/test_ring_small: tests/test_ring.c $(CORE) $(POSIX) $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(ALLFLAGS) -DTBLOG_LITE_BUF_SIZE=301 -DTBLOG_LITE_LINE_MAX=100 -o $@ tests/test_ring.c $(CORE) $(POSIX) -pthread

$(B)/test_isr: tests/test_isr.c $(CORE) $(POSIX) $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(ALLFLAGS) -o $@ tests/test_isr.c $(CORE) $(POSIX) -pthread

$(B)/demo: port/posix/demo.c $(CORE) $(POSIX) $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(ALLFLAGS) -o $@ port/posix/demo.c $(CORE) $(POSIX) -pthread

demo: $(B)/demo
	./$(B)/demo

TESTS := $(B)/test_printf $(B)/test_printf_float $(B)/test_ring $(B)/test_ring_small $(B)/test_isr

# Every tests/test_*.c must be built into at least one of $(TESTS) (a binary
# named after it, possibly with a suffix such as test_ring_small), so a new
# test file cannot be forgotten by `make test`.
UNLISTED_TESTS := $(foreach t,$(wildcard tests/test_*.c),$(if $(filter $(B)/$(basename $(notdir $(t)))%,$(TESTS)),,$(t)))

test: $(TESTS) check-ports
	@if [ -n "$(strip $(UNLISTED_TESTS))" ]; then echo "tblog-lite: not in TESTS: $(UNLISTED_TESTS)"; exit 1; fi
	@set -e; for t in $(TESTS); do echo "=== $$t"; ./$$t; done
	sh tests/check_strip.sh $(CC) $(B)/strip
	@echo "tblog-lite: all tests passed$(if $(SAN), (sanitizers))"

test-asan:
	$(MAKE) SAN=1 test

# RTOS / bare-metal ports cannot run here: compile them for syntax and types
# against stub headers, in every configuration.
SYNTAX := $(WFLAGS) -fsyntax-only -Iinclude -Iport/stubs
check-ports:
	$(CC) $(SYNTAX) -Iport/freertos port/freertos/tblog_lite_port_freertos.c
	$(CC) $(SYNTAX) -DTBLOG_LITE_CM_TIMESTAMP=0 -DTBLOG_LITE_CM_OUTPUT=0 port/cortex_m_baremetal/tblog_lite_port_cortex_m.c
	$(CC) $(SYNTAX) -DTBLOG_LITE_CM_TIMESTAMP=1 -DTBLOG_LITE_CM_OUTPUT=1 port/cortex_m_baremetal/tblog_lite_port_cortex_m.c
	$(CC) $(SYNTAX) '-DTBLOG_LITE_CM_GET_TICK()=0u' port/cortex_m_baremetal/tblog_lite_port_cortex_m.c
	@echo "check-ports: freertos + cortex_m_baremetal syntax ok (untested on hardware)"

# Footprint: text = flash, data+bss = RAM (bss includes the ring buffer).
# No unwind tables on the host, as in a typical arm-none-eabi C build.
SIZEFLAGS := -Os -fno-asynchronous-unwind-tables
size:
	@mkdir -p build/size
	@for f in 0 1; do \
	  $(CC) $(WFLAGS) $(SIZEFLAGS) -Iinclude -DTBLOG_LITE_FLOAT=$$f -c src/tblog_lite.c -o build/size/core.o && \
	  $(CC) $(WFLAGS) $(SIZEFLAGS) -Iinclude -DTBLOG_LITE_FLOAT=$$f -c src/tblog_lite_printf.c -o build/size/printf.o && \
	  echo "== host $$($(CC) -dumpmachine) -Os, TBLOG_LITE_FLOAT=$$f" && size -t build/size/core.o build/size/printf.o; \
	done
	@if command -v $(ARMCC) >/dev/null 2>&1; then \
	  for f in 0 1; do \
	    $(ARMCC) $(WFLAGS) -Os -mcpu=cortex-m4 -mthumb -Iinclude -DTBLOG_LITE_FLOAT=$$f -c src/tblog_lite.c -o build/size/cm4-core.o && \
	    $(ARMCC) $(WFLAGS) -Os -mcpu=cortex-m4 -mthumb -Iinclude -DTBLOG_LITE_FLOAT=$$f -c src/tblog_lite_printf.c -o build/size/cm4-printf.o && \
	    echo "== cortex-m4 thumb -Os, TBLOG_LITE_FLOAT=$$f" && size -t build/size/cm4-core.o build/size/cm4-printf.o; \
	  done; \
	else echo "== $(ARMCC) not found: no Cortex-M numbers"; fi

clean:
	rm -rf build build-san
