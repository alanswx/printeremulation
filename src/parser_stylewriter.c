// Apple StyleWriter emulation (StyleWriter II, Color StyleWriter 1500 / 2500).
//
// Unlike the ImageWriter, the StyleWriter is bidirectional: the host polls the
// printer with status queries and waits for the replies, and there is no flow
// control. The daemon therefore has to answer on the serial port (job->reply_fd).
//
// Protocol (reverse engineered; see references/lpstyl/README.protocol):
//   FF FF FF x   status query, printer replies with one byte ('I' and 'S' excepted)
//   ?            identify: printer replies "CS\r" (color models) or "SW\r"
//   R|c <rect> G <size> <data> 00
//                band of image data; rect is left,top,right,bottom as LE16
//                printer coordinates (inclusive), 'c' = CMYK planes, 'R' = mono
//   0C           end of page
//   m<1 byte>, and single-byte L, F, A, D, E, N, Z, H, B, s, n, t, x, l, h:
//                setup / page / mode commands, no reply (see docs/stylewriter_driver_map.md)
//
// Band data is one encoded row per plane (C, M, Y, K for color bands), each XORed
// with the previous row of the same plane; the XOR history resets every band.
//   01-3F  n literal bytes follow        80     rest of the row unchanged
//   81-BE  n unchanged bytes (white)     C1-FE  n inverted bytes (black)
// The encoding never produces FF, so FF FF FF queries are unambiguous even
// inside a band.

#include "printer.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SW_DPI        360
#define SW_MAX_ROWB   512   // bytes per plane row (letter/A4 need < 400)
#define SW_PLANES     4     // C, M, Y, K

enum { PL_C, PL_M, PL_Y, PL_K };

typedef enum {
    SW_TOP,        // between commands
    SW_RECT,       // collecting the 8 rect bytes after 'R' / 'c'
    SW_WANT_G,     // expecting 'G' after the rect
    SW_SIZE,       // collecting the 2 size bytes after 'G'
    SW_DATA,       // encoded band data
    SW_WANT_NUL,   // band terminator
    SW_SKIP        // discarding command parameters
} SWState;

static struct {
    PrinterModel variant;
    SWState state;
    int ff_count;
    int skip;

    uint8_t hdr[8];
    int hdr_len;
    bool band_color;
    int left, top, right, bottom;
    long data_left;

    // Band decoding
    int rowbytes;
    int nplanes;
    int plane;              // index into plane_order
    int pos;                // byte position in the current plane row
    int lit_left;           // literal bytes still to come
    int row;                // row within the band
    uint8_t rows[SW_PLANES][SW_MAX_ROWB];  // current row per plane (XOR history)

    // Downsampling accumulator for one canvas row
    int acc_cy;             // canvas row being accumulated, -1 = none
    uint16_t *acc[SW_PLANES];
    int acc_width;
} sw;

// Printable-area origin on the page, in 360 dpi dots. lpstyl trims the left
// margin in whole bytes: 90 dots on the StyleWriter II -> 12 bytes = 96 dots.
static int margin_left(void) { return (sw.variant == MODEL_SW2) ? 96 : 72; }
static int margin_top(void)  { return 90; }

static void reply(JobState *job, const char *buf, int len) {
    if (job->reply_fd >= 0) {
        ssize_t w = write(job->reply_fd, buf, (size_t)len);
        (void)w;
    }
}

static void reply_byte(JobState *job, uint8_t v) {
    char c = (char)v;
    reply(job, &c, 1);
}

// Number of 360 dpi dots that land on canvas cell n along one axis
static int cell_span(int n, int dpi) {
    int a = (n * SW_DPI + dpi - 1) / dpi;
    int b = ((n + 1) * SW_DPI + dpi - 1) / dpi;
    return (b > a) ? (b - a) : 1;
}

static void acc_flush(Canvas *c) {
    if (sw.acc_cy < 0 || !sw.acc[0]) return;
    int nrows = cell_span(sw.acc_cy, c->dpi);
    for (int cx = 0; cx < sw.acc_width; cx++) {
        uint16_t k = sw.acc[PL_K][cx], cc = sw.acc[PL_C][cx];
        uint16_t m = sw.acc[PL_M][cx], y = sw.acc[PL_Y][cx];
        if (!(k | cc | m | y)) continue;
        float area = (float)(cell_span(cx, c->dpi) * nrows);
        float fk = k / area, fc = cc / area, fm = m / area, fy = y / area;
        if (fk > 1) fk = 1;
        if (fc > 1) fc = 1;
        if (fm > 1) fm = 1;
        if (fy > 1) fy = 1;
        RGBColor col = {
            (uint8_t)(255.0f * (1 - fc) * (1 - fk) + 0.5f),
            (uint8_t)(255.0f * (1 - fm) * (1 - fk) + 0.5f),
            (uint8_t)(255.0f * (1 - fy) * (1 - fk) + 0.5f)
        };
        canvas_plot_dot(c, cx, sw.acc_cy, col);
    }
    for (int p = 0; p < SW_PLANES; p++)
        memset(sw.acc[p], 0, (size_t)sw.acc_width * sizeof(uint16_t));
    sw.acc_cy = -1;
}

// Add one fully decoded printer row (all planes) to the canvas
static void render_row(Canvas *c) {
    int page_y = margin_top() + sw.top + sw.row;
    int cy = (int)((long)page_y * c->dpi / SW_DPI);
    if (cy != sw.acc_cy) {
        acc_flush(c);
        sw.acc_cy = cy;
    }
    int x0 = margin_left() + sw.left;
    for (int p = 0; p < SW_PLANES; p++) {
        if (!sw.band_color && p != PL_K) continue;
        const uint8_t *r = sw.rows[p];
        for (int i = 0; i < sw.rowbytes; i++) {
            if (!r[i]) continue;
            for (int bit = 0; bit < 8; bit++) {
                if (!(r[i] & (0x80 >> bit))) continue;
                int cx = (int)((long)(x0 + i * 8 + bit) * c->dpi / SW_DPI);
                if (cx >= 0 && cx < sw.acc_width) sw.acc[p][cx]++;
            }
        }
    }
}

static int cur_plane(void) {
    static const int order_color[SW_PLANES] = { PL_C, PL_M, PL_Y, PL_K };
    return sw.band_color ? order_color[sw.plane] : PL_K;
}

// One plane row finished: advance to the next plane, or render the row
static void end_plane_row(Canvas *c) {
    sw.pos = 0;
    sw.lit_left = 0;
    if (++sw.plane < sw.nplanes) return;
    sw.plane = 0;
    render_row(c);
    sw.row++;
}

static void decode_data_byte(Canvas *c, uint8_t b) {
    uint8_t *r = sw.rows[cur_plane()];
    if (sw.lit_left > 0) {
        if (sw.pos < sw.rowbytes) r[sw.pos] ^= b;
        sw.pos++;
        sw.lit_left--;
    } else if (b >= 0x01 && b <= 0x3F) {
        sw.lit_left = b;
        return;
    } else if (b == 0x80) {
        sw.pos = sw.rowbytes;           // rest of the row unchanged
    } else if (b > 0x80 && b < 0xC0) {
        sw.pos += b - 0x80;             // unchanged run
    } else if (b > 0xC0) {
        int n = b - 0xC0;               // inverted run
        for (int i = 0; i < n; i++, sw.pos++)
            if (sw.pos < sw.rowbytes) r[sw.pos] ^= 0xFF;
    }
    if (sw.lit_left == 0 && sw.pos >= sw.rowbytes) end_plane_row(c);
}

static void begin_band(void) {
    sw.left   = sw.hdr[0] | (sw.hdr[1] << 8);
    sw.top    = sw.hdr[2] | (sw.hdr[3] << 8);
    sw.right  = sw.hdr[4] | (sw.hdr[5] << 8);
    sw.bottom = sw.hdr[6] | (sw.hdr[7] << 8);
    sw.rowbytes = (sw.right - sw.left + 1 + 7) / 8;
    if (sw.rowbytes < 1) sw.rowbytes = 1;
    if (sw.rowbytes > SW_MAX_ROWB) sw.rowbytes = SW_MAX_ROWB;
    sw.nplanes = sw.band_color ? SW_PLANES : 1;
    sw.plane = 0;
    sw.pos = 0;
    sw.lit_left = 0;
    sw.row = 0;
    memset(sw.rows, 0, sizeof(sw.rows));   // XOR history restarts each band
}

static void handle_query(JobState *job, uint8_t q) {
    uint8_t v = 0x00;
    switch (q) {
        case 'I':                       // eject and reset: no reply
            sw.state = SW_TOP;
            return;
        case 'S':                       // resume after paper out: no reply
            return;
        case '1': v = 0x00; break;      // not busy
        case '2': v = 0x00; break;      // no error
        case 'B':                       // buffer status: always drained
            v = (sw.variant == MODEL_SW2500) ? 0x80 :
                (sw.variant == MODEL_SW1500) ? 0x87 : 0xF8;
            break;
        case 'p':                       // Color StyleWriter sub-model
            v = (sw.variant == MODEL_SW2500) ? 0x05 :
                (sw.variant == MODEL_SW1500) ? 0x04 : 0x00;
            break;
        case 'H':                       // cartridge: color installed
            v = (sw.variant == MODEL_SW2) ? 0x01 : 0x81;
            break;
        default: break;
    }
    if (job->verbose) printf("[stylewriter] query '%c' (0x%02X) -> 0x%02X\n", q, q, v);
    reply_byte(job, v);
}

static void state_byte(JobState *job, uint8_t b) {
    Canvas *c = &job->canvas;
    switch (sw.state) {
        case SW_TOP:
            switch (b) {
                case '?': {
                    const char *id = (sw.variant == MODEL_SW2) ? "SW\r" : "CS\r";
                    if (job->verbose) printf("[stylewriter] identify -> %.2s\n", id);
                    reply(job, id, 3);
                    break;
                }
                case 'R':
                case 'c':
                    sw.band_color = (b == 'c');
                    sw.hdr_len = 0;
                    sw.state = SW_RECT;
                    break;
                case 'm':               // m<digit>: print mode, one parameter byte
                    sw.skip = 1;
                    sw.state = SW_SKIP;
                    break;
                case 0x0C:              // end of page
                    acc_flush(c);
                    job_commit_page(job);
                    break;
                default:                // L, n, u, A, D, N, Z, H, B ...: no reply
                    break;
            }
            break;

        case SW_RECT:
            sw.hdr[sw.hdr_len++] = b;
            if (sw.hdr_len == 8) sw.state = SW_WANT_G;
            break;

        case SW_WANT_G:
            if (b == 'G') {
                sw.hdr_len = 0;
                sw.state = SW_SIZE;
            } else {
                sw.state = SW_TOP;
                state_byte(job, b);
            }
            break;

        case SW_SIZE:
            if (sw.hdr_len == 0) {
                sw.data_left = b;
                sw.hdr_len = 1;
            } else {
                sw.data_left |= (long)b << 8;
                // The rect bytes were saved in hdr[] before 'G'
                begin_band();
                if (job->verbose)
                    printf("[stylewriter] band %s rect (%d,%d)-(%d,%d), %ld bytes\n",
                           sw.band_color ? "CMYK" : "mono",
                           sw.left, sw.top, sw.right, sw.bottom, sw.data_left);
                sw.state = (sw.data_left > 0) ? SW_DATA : SW_WANT_NUL;
            }
            break;

        case SW_DATA:
            decode_data_byte(c, b);
            if (--sw.data_left <= 0) sw.state = SW_WANT_NUL;
            break;

        case SW_WANT_NUL:
            sw.state = SW_TOP;
            if (b != 0x00) state_byte(job, b);
            break;

        case SW_SKIP:
            if (--sw.skip <= 0) sw.state = SW_TOP;
            break;
    }
}

void parser_stylewriter_init(JobState *job, PrinterModel variant) {
    Canvas *c = &job->canvas;
    sw.variant = variant;
    sw.state = SW_TOP;
    sw.ff_count = 0;
    sw.acc_cy = -1;
    if (!sw.acc[0] || sw.acc_width != c->width) {
        for (int p = 0; p < SW_PLANES; p++) {
            free(sw.acc[p]);
            sw.acc[p] = (uint16_t *)calloc((size_t)c->width, sizeof(uint16_t));
        }
        sw.acc_width = c->width;
    } else {
        for (int p = 0; p < SW_PLANES; p++)
            memset(sw.acc[p], 0, (size_t)sw.acc_width * sizeof(uint16_t));
    }
}

void parser_stylewriter_byte(JobState *job, uint8_t byte) {
    // FF never occurs in band data, so three of them always start a query. One
    // or two FFs followed by something else were header bytes (rect or size).
    if (byte == 0xFF) {
        sw.ff_count++;
        return;
    }
    if (sw.ff_count >= 3) {
        sw.ff_count = 0;
        handle_query(job, byte);
        return;
    }
    while (sw.ff_count > 0) {
        sw.ff_count--;
        state_byte(job, 0xFF);
    }
    state_byte(job, byte);
}

// Push a partially accumulated canvas row out before the page is committed
void parser_stylewriter_flush(JobState *job) {
    acc_flush(&job->canvas);
}
