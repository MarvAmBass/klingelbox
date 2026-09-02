# host-test/Makefile - build and run the logic tests with plain gcc.
#
# NO ESP-IDF IS INVOLVED, deliberately. The files exercised here are required to
# stay free of IDF and FreeRTOS headers so the interesting logic can be run in a
# millisecond instead of a flash-and-press-a-button cycle. This Makefile failing
# to compile is itself the layering test: if one of them grows an
# `#include "esp_log.h"`, the build breaks here first.
#
# TWO BINARIES, TWO SUBJECTS.
#
#   test_rf_decode   the protocol-agnostic pulse logic in components/rfpulse:
#                    base-width estimation, confidence, fingerprint stability
#                    under oscillator drift, EV1527 framing. Links ONLY rfpulse.
#
#   test_node_graph  the two pieces of main/ that can destroy a user's data and
#                    have no hardware in them: the frozen on-flash node layouts
#                    and their widening (node_migrate.c), and the one MQTT topic
#                    validity rule every call site shares (mqtt_topic.c).
#                    node_graph.h needs two type declarations from the framework
#                    and nothing more, so it compiles against stubs/ rather than
#                    against ESP-IDF. If those stubs ever stop being enough, the
#                    header has grown a real dependency and this net has a hole.
#
# They are separate binaries rather than one because the rfpulse test's whole
# premise is that it links three files and no others.
#
#   make test    build and run both
#   make clean
#
RFPULSE  := ../components/rfpulse
MAIN     := ../main

CC       ?= gcc
CFLAGS   ?= -Wall -Wextra -std=c99 -O2 -g
CPPFLAGS += -I$(RFPULSE)/include

RF_SRCS := test_rf_decode.c \
        $(RFPULSE)/rf_frame.c \
        $(RFPULSE)/rf_decode.c \
        $(RFPULSE)/rf_ev1527.c \
        $(RFPULSE)/rf_group.c

NG_SRCS := test_node_graph.c \
        $(MAIN)/node_migrate.c \
        $(MAIN)/mqtt_topic.c

BINS := test_rf_decode test_node_graph

.PHONY: all test clean
all: $(BINS)

test_rf_decode: $(RF_SRCS) $(RFPULSE)/include/rf_frame.h $(RFPULSE)/include/rf_decode.h \
        $(RFPULSE)/include/rf_group.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(RF_SRCS)

# -Istubs comes LAST so a real header always wins if one is ever added.
test_node_graph: $(NG_SRCS) $(MAIN)/node_migrate.h $(MAIN)/node_graph.h \
        $(MAIN)/mqtt_topic.h stubs/esp_err.h stubs/driver/gpio.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -I$(MAIN) -Istubs -o $@ $(NG_SRCS)

test: $(BINS)
	./test_rf_decode
	@echo
	./test_node_graph

clean:
	rm -rf $(BINS) $(addsuffix .dSYM,$(BINS))
