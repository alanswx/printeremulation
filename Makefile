CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -Wno-format -Isrc
LDFLAGS ?= -lm

DATE ?= $(shell date +%Y%m%d)

# ARM cross-compilation toolchain detection:
# 1. ARM_CC environment variable if set
# 2. Local toolchain in PATH (arm-none-linux-gnueabihf-gcc or arm-linux-gnueabihf-gcc)
# 3. Standard toolchain location in /opt/gcc-arm*
# 4. Remote build host (e.g. cottageubuntu)
# 5. Docker container fallback

DEFAULT_ARM_PATHS = \
	$(shell which arm-none-linux-gnueabihf-gcc 2>/dev/null) \
	$(shell which arm-linux-gnueabihf-gcc 2>/dev/null) \
	$(wildcard /opt/gcc-arm*/bin/arm-none-linux-gnueabihf-gcc) \
	$(wildcard /opt/gcc-arm*/bin/arm-linux-gnueabihf-gcc)

ARM_CC ?= $(firstword $(DEFAULT_ARM_PATHS))
ifeq ($(ARM_CC),)
	ARM_CC := arm-none-linux-gnueabihf-gcc
endif

ARM_STRIP ?= $(patsubst %gcc,%strip,$(ARM_CC))
ifeq ($(ARM_STRIP),$(ARM_CC))
	ARM_STRIP := arm-none-linux-gnueabihf-strip
endif

ARM_CC_EXISTS := $(shell which $(ARM_CC) 2>/dev/null || [ -x "$(ARM_CC)" ] && echo "yes")
DOCKER_IMAGE ?= mister-build:latest
REMOTE_BUILD_HOST ?= cottageubuntu
REMOTE_TOOLCHAIN ?= /opt/gcc-arm-10.2-2020.11-x86_64-arm-none-linux-gnueabihf/bin

SRCS = $(wildcard src/*.c)
HEADERS = $(wildcard src/*.h)

TARGET = build/mister_printerd
ARM_TARGET = build/mister_printerd.arm
RELEASE_BINARY = releases/mister_printerd_$(DATE)
RELEASE_SYMLINK = releases/mister_printerd

.PHONY: all arm release deploy test test-stylewriter-lpstyl clean

all: $(TARGET)

$(TARGET): $(SRCS) $(HEADERS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRCS) $(LDFLAGS) -o $(TARGET)
	@echo "Built host binary: $(TARGET)"

arm: $(ARM_TARGET)

$(ARM_TARGET): $(SRCS) $(HEADERS)
	@mkdir -p build
	@if [ -n "$(ARM_CC_EXISTS)" ]; then \
		echo "Building with local toolchain $(ARM_CC)..."; \
		$(ARM_CC) $(CFLAGS) $(SRCS) $(LDFLAGS) -o $(ARM_TARGET); \
		$(ARM_STRIP) $(ARM_TARGET); \
	elif which docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then \
		echo "Host ARM compiler not found, building with Docker ($(DOCKER_IMAGE))..."; \
		docker run --rm -v "$$(pwd)":/work -w /work $(DOCKER_IMAGE) sh -c \
			"arm-none-linux-gnueabihf-gcc $(CFLAGS) $(SRCS) $(LDFLAGS) -o $(ARM_TARGET) && arm-none-linux-gnueabihf-strip $(ARM_TARGET)"; \
	elif ssh -q -o BatchMode=yes -o ConnectTimeout=2 $(REMOTE_BUILD_HOST) true 2>/dev/null; then \
		echo "Building on remote host $(REMOTE_BUILD_HOST) via $(REMOTE_TOOLCHAIN)..."; \
		ssh $(REMOTE_BUILD_HOST) "mkdir -p /tmp/build_mister_printerd"; \
		scp -r src/* $(REMOTE_BUILD_HOST):/tmp/build_mister_printerd/; \
		ssh $(REMOTE_BUILD_HOST) "cd /tmp/build_mister_printerd && $(REMOTE_TOOLCHAIN)/arm-none-linux-gnueabihf-gcc $(CFLAGS) -I. *.c $(LDFLAGS) -o mister_printerd && $(REMOTE_TOOLCHAIN)/arm-none-linux-gnueabihf-strip mister_printerd"; \
		scp $(REMOTE_BUILD_HOST):/tmp/build_mister_printerd/mister_printerd $(ARM_TARGET); \
	else \
		echo "Error: No ARM cross compiler, running Docker, or reachable remote build host found."; \
		exit 1; \
	fi
	@echo "Built and stripped ARM binary: $(ARM_TARGET)"

release: $(ARM_TARGET)
	@mkdir -p releases
	cp $(ARM_TARGET) $(RELEASE_BINARY)
	cp $(ARM_TARGET) $(RELEASE_SYMLINK)
	@echo "Created release: $(RELEASE_BINARY) and $(RELEASE_SYMLINK)"

deploy: arm
	scp $(ARM_TARGET) root@mister.local:/media/fat/mister_printerd
	ssh root@mister.local "chmod +x /media/fat/mister_printerd && mkdir -p /media/fat/printers"
	@echo "Successfully deployed to root@mister.local:/media/fat/mister_printerd"

test: $(TARGET)
	@mkdir -p printers tests/data
	@echo "1. Generating test streams..."
	python3 tests/generate_test_streams.py
	@echo "2. Testing ImageWriter with raw Print Shop dump..."
	$(TARGET) -d tests/samples/imagewriter_printshop.txt -m imagewriter -t 1 -v
	@echo "3. Testing ImageWriter II Color..."
	$(TARGET) -d tests/data/test_imagewriter_color.prn -m imagewriter -t 1 -v
	@echo "3b. Testing ImageWriter II from GS/OS Print Manager (ESC V column repeats, captured on MiSTer)..."
	$(TARGET) -d tests/samples/imagewriter_gsos_hermes.prn -m imagewriter -t 1 -v
	@echo "4. Testing Epson ESC/P (Print Shop TPS mode)..."
	$(TARGET) -d tests/data/test_escp.prn -m epson-tps -t 1 -v
	@echo "5. Testing Coleco Adam SmartWriter (streams captured from the ColecoAdam core)..."
	$(TARGET) -d tests/samples/adam_smartwriter_typewriter.prn -m adam -t 1 -v
	$(TARGET) -d tests/samples/adam_smartwriter_wordproc.prn -c Adam -t 1 -v
	$(TARGET) -d tests/samples/adam_smartwriter_superscript.prn -c Adam -t 1 -v
	@echo "6. Testing Commodore MPS 803..."
	$(TARGET) -d tests/data/test_mps803.prn -m mps803 -t 1 -v
	@echo "7. Testing Apple StyleWriter (decoded dots compared against the source page)..."
	python3 tests/stylewriter_loopback.py --host python --model stylewriter2500
	python3 tests/stylewriter_loopback.py --host python --model stylewriter2500 --color
	python3 tests/stylewriter_loopback.py --host python --model stylewriter1500 --color
	python3 tests/stylewriter_loopback.py --host python --model stylewriter2
	@echo "All tests passed successfully! Generated PDFs:"
	@ls -lh printers/*.pdf

# StyleWriter emulation driven by the real lpstyl host driver over a pty.
# Needs references/lpstyl (git clone https://github.com/Godzil/lpstyl references/lpstyl).
LPSTYL = build/lpstyl
$(LPSTYL): references/lpstyl/lpstyl.c
	@mkdir -p build
	$(CC) -O2 -w -std=gnu89 -include unistd.h -include fcntl.h -include string.h \
		-include stdlib.h -include errno.h -include signal.h -include termios.h -o $@ $<

test-stylewriter-lpstyl: $(TARGET) $(LPSTYL)
	python3 tests/stylewriter_loopback.py --host lpstyl --model stylewriter2500
	python3 tests/stylewriter_loopback.py --host lpstyl --model stylewriter1500
	python3 tests/stylewriter_loopback.py --host lpstyl --model stylewriter2

clean:
	rm -rf build printers/*.pdf printers/*.png
