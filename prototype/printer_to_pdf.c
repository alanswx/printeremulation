/**
 * MiSTer Printer Emulation - Proof of Concept
 * Supports Apple ImageWriter I/II & Epson ESC/P to PDF conversion
 * Built using single-file PDFGen (Public Domain)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include "pdfgen.h"

#define DPI 144
#define PAGE_WIDTH_INCH  8.5f
#define PAGE_HEIGHT_INCH 11.0f

#define CANVAS_WIDTH  ((int)(PAGE_WIDTH_INCH * DPI))   // 1224 px
#define CANVAS_HEIGHT ((int)(PAGE_HEIGHT_INCH * DPI))  // 1584 px

typedef enum {
    PRINTER_IMAGEWRITER,
    PRINTER_EPSON
} PrinterType;

typedef struct {
    uint8_t r, g, b;
} RGBColor;

static const RGBColor IW_COLORS[8] = {
    {0, 0, 0},        // 0: Black
    {255, 220, 0},    // 1: Yellow
    {227, 27, 35},    // 2: Red
    {0, 128, 255},    // 3: Blue
    {255, 128, 0},    // 4: Orange (Yellow + Red)
    {0, 176, 80},     // 5: Green (Yellow + Blue)
    {112, 48, 160},   // 6: Purple (Red + Blue)
    {0, 0, 0}         // 7: Default Black
};

typedef struct {
    PrinterType type;
    int head_x;        // in canvas pixels
    int head_y;        // in canvas pixels
    int line_spacing;  // in 1/144 inch units (canvas pixels)
    RGBColor cur_color;
    uint8_t *canvas;   // RGB24 buffer [CANVAS_HEIGHT * CANVAS_WIDTH * 3]
    struct pdf_doc *pdf;
    int page_count;
    int page_dirty;
} PrinterState;

static void clear_canvas(PrinterState *p) {
    // Fill with white (255, 255, 255)
    memset(p->canvas, 0xFF, (size_t)CANVAS_WIDTH * CANVAS_HEIGHT * 3);
    p->head_x = (int)(0.5f * DPI); // 0.5 inch left margin
    p->head_y = (int)(0.5f * DPI); // 0.5 inch top margin
    p->page_dirty = 0;
}

static void init_printer(PrinterState *p, PrinterType type) {
    p->type = type;
    p->line_spacing = (144 / 6); // default 1/6 inch = 24 dots at 144 DPI
    p->cur_color = IW_COLORS[0]; // Black
    p->canvas = (uint8_t *)malloc((size_t)CANVAS_WIDTH * CANVAS_HEIGHT * 3);
    if (!p->canvas) {
        fprintf(stderr, "Error: Failed to allocate canvas memory.\n");
        exit(1);
    }
    clear_canvas(p);

    struct pdf_info info = {
        .creator = "MiSTer FPGA Retro Printer Emulation",
        .producer = "mister_printerd (PDFGen)",
        .title = "Retro Printout",
        .author = "Apple II / IIgs",
        .subject = "Emulated Dot Matrix Printout",
        .date = "2026"
    };

    // Letter size: 612 x 792 points (72 points/inch)
    p->pdf = pdf_create(PDF_LETTER_WIDTH, PDF_LETTER_HEIGHT, &info);
    p->page_count = 0;
}

static void plot_dot(PrinterState *p, int x, int y, RGBColor col) {
    if (x < 0 || x >= CANVAS_WIDTH || y < 0 || y >= CANVAS_HEIGHT) return;
    int idx = (y * CANVAS_WIDTH + x) * 3;
    // Subtractive / blend dot onto canvas
    p->canvas[idx + 0] = (uint8_t)((p->canvas[idx + 0] * col.r) / 255);
    p->canvas[idx + 1] = (uint8_t)((p->canvas[idx + 1] * col.g) / 255);
    p->canvas[idx + 2] = (uint8_t)((p->canvas[idx + 2] * col.b) / 255);
    p->page_dirty = 1;
}

static void flush_page(PrinterState *p) {
    if (!p->page_dirty) return;

    printf("Flushing Page %d to PDF (%dx%d px @ %d DPI)...\n",
           p->page_count + 1, CANVAS_WIDTH, CANVAS_HEIGHT, DPI);

    struct pdf_object *page = pdf_append_page(p->pdf);
    // PDF coordinates have origin at bottom-left, while raster has origin at top-left.
    // pdf_add_rgb24 places the image correctly at (x, y) with (width, height) in points.
    pdf_add_rgb24(p->pdf, page, 0, 0, PDF_LETTER_WIDTH, PDF_LETTER_HEIGHT,
                  p->canvas, CANVAS_WIDTH, CANVAS_HEIGHT);

    p->page_count++;
    clear_canvas(p);
}

// Parse ImageWriter stream
static void parse_imagewriter(PrinterState *p, const uint8_t *data, size_t len) {
    size_t i = 0;
    while (i < len) {
        uint8_t b = data[i++];
        if (b == 0x0D) { // Carriage Return
            p->head_x = (int)(0.5f * DPI);
        } else if (b == 0x0A) { // Line Feed
            p->head_y += p->line_spacing;
            if (p->head_y >= CANVAS_HEIGHT - (int)(0.5f * DPI)) {
                flush_page(p);
            }
        } else if (b == 0x0C) { // Form Feed
            flush_page(p);
        } else if (b == 0x1B) { // ESC sequence
            if (i >= len) break;
            uint8_t cmd = data[i++];
            if (cmd == 'T') {
                // ESC T nn : line spacing in nn/144"
                if (i + 1 < len && isdigit(data[i]) && isdigit(data[i+1])) {
                    int nn = (data[i] - '0') * 10 + (data[i+1] - '0');
                    p->line_spacing = nn; // since canvas is 144 DPI, nn/144" == nn pixels!
                    i += 2;
                }
            } else if (cmd == 'K') {
                // ESC K c : Color ribbon selection
                if (i < len) {
                    uint8_t c = data[i++];
                    int col_idx = (c >= '0' && c <= '6') ? (c - '0') : 0;
                    p->cur_color = IW_COLORS[col_idx];
                }
            } else if (cmd == 'G' || cmd == 'P' || cmd == 'S') {
                // ESC G / P / S d1 d2 d3 d4 <bytes>
                // G = 72 DPI (2 px wide at 144 DPI), P/S = 144 DPI (1 px wide)
                int scale_x = (cmd == 'G') ? 2 : 1;
                if (i + 3 < len && isdigit(data[i]) && isdigit(data[i+1]) &&
                    isdigit(data[i+2]) && isdigit(data[i+3])) {
                    int cols = (data[i] - '0') * 1000 + (data[i+1] - '0') * 100 +
                               (data[i+2] - '0') * 10 + (data[i+3] - '0');
                    i += 4;
                    for (int c = 0; c < cols && i < len; c++) {
                        uint8_t slice = data[i++];
                        // Bit 7 is top dot (Pin 1), Bit 0 is bottom dot (Pin 8)
                        for (int pin = 0; pin < 8; pin++) {
                            if (slice & (1 << (7 - pin))) {
                                int py = p->head_y + (pin * (144 / 72)); // 72 DPI vertical pin spacing = 2 px
                                int px = p->head_x;
                                plot_dot(p, px, py, p->cur_color);
                                if (scale_x == 2) {
                                    plot_dot(p, px + 1, py, p->cur_color);
                                }
                            }
                        }
                        p->head_x += scale_x;
                    }
                }
            }
            // Ignore other escape commands for now (e.g. font pitch, margins)
        }
    }
}

// Parse Epson ESC/P stream
static void parse_escp(PrinterState *p, const uint8_t *data, size_t len) {
    size_t i = 0;
    while (i < len) {
        uint8_t b = data[i++];
        if (b == 0x0D) { // CR
            p->head_x = (int)(0.5f * DPI);
        } else if (b == 0x0A) { // LF
            p->head_y += p->line_spacing;
            if (p->head_y >= CANVAS_HEIGHT - (int)(0.5f * DPI)) {
                flush_page(p);
            }
        } else if (b == 0x0C) { // FF
            flush_page(p);
        } else if (b == 0x1B) { // ESC
            if (i >= len) break;
            uint8_t cmd = data[i++];
            if (cmd == '@') {
                // ESC @ : Initialize
                p->line_spacing = (144 / 6);
                p->head_x = (int)(0.5f * DPI);
            } else if (cmd == '3') {
                // ESC 3 n : n/216" line spacing
                if (i < len) {
                    uint8_t n = data[i++];
                    p->line_spacing = (int)((n * 144.0f) / 216.0f + 0.5f);
                }
            } else if (cmd == 'A') {
                // ESC A n : n/72" line spacing
                if (i < len) {
                    uint8_t n = data[i++];
                    p->line_spacing = (int)((n * 144.0f) / 72.0f + 0.5f);
                }
            } else if (cmd == '2') {
                // ESC 2 : 1/6" line spacing
                p->line_spacing = 144 / 6;
            } else if (cmd == '0') {
                // ESC 0 : 1/8" line spacing
                p->line_spacing = 144 / 8;
            } else if (cmd == 'K' || cmd == 'L' || cmd == 'Y' || cmd == 'Z') {
                // 8-pin graphics: ESC <K/L/Y/Z> nL nH
                if (i + 1 < len) {
                    uint8_t nL = data[i++];
                    uint8_t nH = data[i++];
                    int cols = nL | (nH << 8);
                    // K=60 DPI, L/Y=120 DPI, Z=240 DPI
                    float step_x = (cmd == 'K') ? (144.0f / 60.0f) :
                                   (cmd == 'Z') ? (144.0f / 240.0f) : (144.0f / 120.0f);

                    for (int c = 0; c < cols && i < len; c++) {
                        uint8_t slice = data[i++];
                        for (int pin = 0; pin < 8; pin++) {
                            if (slice & (1 << (7 - pin))) {
                                int py = p->head_y + (pin * (144 / 72));
                                int px = (int)(p->head_x + c * step_x);
                                plot_dot(p, px, py, IW_COLORS[0]);
                                if (step_x >= 2.0f) {
                                    plot_dot(p, px + 1, py, IW_COLORS[0]);
                                }
                            }
                        }
                    }
                    p->head_x += (int)(cols * step_x);
                }
            }
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: %s <imagewriter|epson> <input_file> [output_pdf]\n", argv[0]);
        return 1;
    }

    const char *mode = argv[1];
    const char *in_filename = argv[2];
    const char *out_filename = (argc >= 4) ? argv[3] : "output.pdf";

    PrinterType type = (strcasecmp(mode, "epson") == 0) ? PRINTER_EPSON : PRINTER_IMAGEWRITER;

    FILE *fin = fopen(in_filename, "rb");
    if (!fin) {
        perror("fopen input");
        return 1;
    }

    fseek(fin, 0, SEEK_END);
    long sz = ftell(fin);
    fseek(fin, 0, SEEK_SET);

    uint8_t *buf = (uint8_t *)malloc(sz);
    if (fread(buf, 1, sz, fin) != (size_t)sz) {
        perror("fread");
        fclose(fin);
        free(buf);
        return 1;
    }
    fclose(fin);

    printf("Read %ld bytes from %s.\n", sz, in_filename);

    PrinterState state;
    init_printer(&state, type);

    if (type == PRINTER_IMAGEWRITER) {
        parse_imagewriter(&state, buf, sz);
    } else {
        parse_escp(&state, buf, sz);
    }

    // Flush any trailing page if there was uncommitted data
    flush_page(&state);

    if (state.page_count == 0) {
        printf("Warning: No pages printed, creating blank page.\n");
        struct pdf_object *page = pdf_append_page(state.pdf);
        pdf_add_rgb24(state.pdf, page, 0, 0, PDF_LETTER_WIDTH, PDF_LETTER_HEIGHT,
                      state.canvas, CANVAS_WIDTH, CANVAS_HEIGHT);
    }

    int ret = pdf_save(state.pdf, out_filename);
    if (ret < 0) {
        fprintf(stderr, "Error saving PDF: %s\n", pdf_get_err(state.pdf, NULL));
    } else {
        printf("Successfully generated PDF: %s (%d pages)\n", out_filename, state.page_count);
    }

    pdf_destroy(state.pdf);
    free(state.canvas);
    free(buf);
    return 0;
}
