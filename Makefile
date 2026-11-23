# Build and test with nothing installed but Docker.
#
# The toolchain arrives as the same prebuilt image CI uses
# (espressif/idf:v5.3.1, about 6 GB on disk after the first pull), so "it
# builds here" and "it builds in CI" stay one statement. The host tests need
# only gcc and make, which that image also carries — one image for everything.
#
#   make build    firmware + web UI images        -> firmware/build/
#   make test     host logic tests (no ESP-IDF involved, seconds not minutes)
#   make clean    idf.py fullclean
#   make shell    interactive shell inside the toolchain image
#
# Flashing a bare board needs no toolchain at all — use the browser flasher
# (see README). A running box updates itself over HTTP via /api/ota/.
#
# A locally installed ESP-IDF keeps working exactly as before: this file only
# wraps `idf.py` in a container, it does not replace it.

IDF_VERSION ?= v5.3.1
IDF_IMAGE   ?= espressif/idf:$(IDF_VERSION)

# -t only when stdin is a terminal — CI and scripts have none, and a forced
# TTY there makes docker abort before the build even starts.
TTY = $(shell [ -t 0 ] && echo -t)
RUN = docker run --rm -i $(TTY) -v "$(CURDIR)":/project -w /project/firmware $(IDF_IMAGE)

.PHONY: build test clean shell

build:
	$(RUN) idf.py build

test:
	$(RUN) make -C host-test test

clean:
	$(RUN) idf.py fullclean

shell:
	$(RUN) bash
