CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -Ireferences/PDFGen -Isrc
LDFLAGS ?= -lm

ARM_CC ?= arm-linux-gnueabihf-gcc

SRCS = src/mister_printerd.c \
       src/canvas.c \
       src/pdf_writer.c \
       src/parser_imagewriter.c \
       src/parser_escp.c \
       src/parser_adam.c \
       src/parser_mps803.c \
       references/PDFGen/pdfgen.c

OBJS = $(patsubst %.c,build/%.o,$(SRCS))
ARM_OBJS = $(patsubst %.c,build_arm/%.o,$(SRCS))

TARGET = build/mister_printerd
ARM_TARGET = build/mister_printerd.arm

all: $(TARGET)

$(TARGET): $(SRCS)
	@mkdir -p build/src build/references/PDFGen
	$(CC) $(CFLAGS) $(SRCS) $(LDFLAGS) -o $(TARGET)
	@echo "Built host binary: $(TARGET)"

arm:
	@mkdir -p build_arm/src build_arm/references/PDFGen
	$(ARM_CC) $(CFLAGS) $(SRCS) $(LDFLAGS) -o $(ARM_TARGET)
	@echo "Built ARM binary: $(ARM_TARGET)"

test: $(TARGET)
	@mkdir -p printers tests/data
	@echo "1. Generating test streams..."
	python3 tests/generate_test_streams.py
	python3 prototype/gen_escp_test.py
	@echo "2. Testing ImageWriter with raw Print Shop dump..."
	$(TARGET) -d references/ImageWriter/Printer.txt -m imagewriter -t 1 -v
	@echo "3. Testing ImageWriter II Color..."
	$(TARGET) -d tests/data/test_imagewriter_color.prn -m imagewriter -t 1 -v
	@echo "4. Testing Epson ESC/P (Print Shop TPS mode)..."
	$(TARGET) -d test_escp.prn -m epson-tps -t 1 -v
	@echo "5. Testing Coleco Adam SmartWriter..."
	$(TARGET) -d tests/data/test_adam.prn -m adam -t 1 -v
	@echo "6. Testing Commodore MPS 803..."
	$(TARGET) -d tests/data/test_mps803.prn -m mps803 -t 1 -v
	@echo "All tests passed successfully! Generated PDFs:"
	@ls -lh printers/*.pdf

clean:
	rm -rf build build_arm printers/*.pdf printers/*.png

.PHONY: all arm test clean
