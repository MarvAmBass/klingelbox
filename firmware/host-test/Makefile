# host-test/Makefile - build and run the rfpulse logic tests with plain gcc.
#
# NO ESP-IDF IS INVOLVED, deliberately. rf_frame.c, rf_decode.c and rf_ev1527.c
# are required to stay free of IDF and FreeRTOS headers so the interesting logic
# (base-width estimation, confidence, fingerprint, EV1527 framing) can be
# exercised in a millisecond instead of a flash-and-press-a-button cycle. This
# Makefile failing to compile is itself the layering test: if one of those three
# files grows an `#include "esp_log.h"`, the build breaks here first.
#
#   make test    build and run
#   make clean
#
RFPULSE  := ../components/rfpulse

CC       ?= gcc
CFLAGS   ?= -Wall -Wextra -std=c99 -O2 -g
CPPFLAGS += -I$(RFPULSE)/include

SRCS := test_rf_decode.c \
        $(RFPULSE)/rf_frame.c \
        $(RFPULSE)/rf_decode.c \
        $(RFPULSE)/rf_ev1527.c

BIN  := test_rf_decode

.PHONY: all test clean
all: $(BIN)

$(BIN): $(SRCS) $(RFPULSE)/include/rf_frame.h $(RFPULSE)/include/rf_decode.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(SRCS)

test: $(BIN)
	./$(BIN)

clean:
	rm -rf $(BIN) $(BIN).dSYM
