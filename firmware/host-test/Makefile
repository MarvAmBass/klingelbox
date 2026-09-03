# host-test/Makefile - build and run the logic tests with plain gcc.
#
# NO ESP-IDF IS INVOLVED, deliberately. The files exercised here can be run in
# a millisecond instead of a flash-and-press-a-button cycle. rfpulse is still
# required to stay free of IDF and FreeRTOS headers and compiles against no
# stubs at all; the persistence sources compile against stubs/ — small fakes of
# the handful of framework pieces they touch, the important one being an
# in-RAM NVS that can be told to fill up or fail (stubs/host_env.c).
#
# TWO BINARIES, TWO SUBJECTS.
#
#   test_rf_decode   the protocol-agnostic pulse logic in components/rfpulse:
#                    base-width estimation, confidence, fingerprint stability
#                    under oscillator drift, EV1527 framing. Links ONLY rfpulse.
#
#   test_version_cmp the release-tag ordering rule (main/version_cmp.h,
#                    header-only): numeric compare plus prerelease-before-
#                    final. Wrong once in a way only an rc runner would ever
#                    see, which is exactly the user no on-device test covers.
#
#   test_node_graph  the pieces of main/ that can destroy a user's data:
#                    the frozen on-flash node layouts and their widening
#                    (node_migrate.c), the shared MQTT topic validity rule
#                    (mqtt_topic.c), and — against the fake NVS — the
#                    node graph's and the signal store's own persistence:
#                    save/rollback discipline, the delete ordering that a
#                    power cut must not turn into a ghost signal, the
#                    boot-time flash reconciliation, and the store-full
#                    budget check. DB_HOSTTEST enables the reset hooks that
#                    let one process simulate several boots.
#
# They are separate binaries rather than one because the rfpulse test's whole
# premise is that it links three files and no others.
#
#   make test    build and run both
#   make clean
#
RFPULSE  := ../components/rfpulse
MAIN     := ../main
CC1101   := ../components/cc1101/include

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
        $(MAIN)/mqtt_topic.c \
        $(MAIN)/node_graph.c \
        $(MAIN)/signal_store.c \
        stubs/host_env.c \
        $(RFPULSE)/rf_frame.c \
        $(RFPULSE)/rf_decode.c \
        $(RFPULSE)/rf_ev1527.c

BINS := test_rf_decode test_node_graph test_version_cmp

.PHONY: all test clean
all: $(BINS)

test_rf_decode: $(RF_SRCS) $(RFPULSE)/include/rf_frame.h $(RFPULSE)/include/rf_decode.h \
        $(RFPULSE)/include/rf_group.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(RF_SRCS)

# -Istubs comes LAST so a real header always wins if one is ever added; the
# stub set is exactly the framework surface node_graph.c/signal_store.c touch,
# and if either grows a NEW framework dependency this build breaks first —
# which is the moment to decide whether it belongs in the testable core at all.
test_node_graph: $(NG_SRCS) $(MAIN)/node_migrate.h $(MAIN)/node_graph.h \
        $(MAIN)/mqtt_topic.h $(MAIN)/signal_store.h stubs/esp_err.h \
        stubs/driver/gpio.h stubs/nvs.h stubs/host_env.h stubs/host_env.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -DDB_HOSTTEST -I$(MAIN) -I$(CC1101) -Istubs \
	        -o $@ $(NG_SRCS)

test_version_cmp: test_version_cmp.c $(MAIN)/version_cmp.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -I$(MAIN) -o $@ test_version_cmp.c

test: $(BINS)
	./test_rf_decode
	@echo
	./test_node_graph
	@echo
	./test_version_cmp

clean:
	rm -rf $(BINS) $(addsuffix .dSYM,$(BINS))
