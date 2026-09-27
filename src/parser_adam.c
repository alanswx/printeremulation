#include "printer.h"
#include <string.h>

#include "font5x7.h"

// Coleco ADAM daisy wheel printer (AdamNet device 2).
//
// The printer holds only 16 characters and EOS sends it plain bytes, so the
// parser models the print head itself: a column at 10 characters per inch
// (ADAM Technical Manual 1-1: "Pitch is 10 characters to the inch"), a vertical
// position in half lines, and a print direction. Characters are struck wherever
// the head is, which covers everything SmartWriter does with it:
//
//   CR   carriage to column 0
//   LF   one line (1/6")
//   VT   half a line (1/12"). SmartWriter feeds each line as VT text VT, and
//        prints superscripts on the half line above the text before the first VT
//   BS   one column back. SmartWriter uses runs of BS, not CR, to return to the
//        margin
//   SO   reverse the head's left and right (Technical Manual 4.2), so characters
//        print right to left. SmartWriter prints some lines backwards
//   SI   back to left to right
//   FF   next sheet
//   ESC  the manual says the printer responds to escape but not how; ESC and the
//        byte after it are dropped
//
// The codes and how SmartWriter uses them were checked against its output
// captured with the ColecoAdam MiSTer core's simulator (--printer).
//
// The paper is treated as continuous (fan-fold): the head starts half an inch
// down the first sheet and a new page begins wherever the feed crosses a sheet
// boundary, so SmartWriter's own page-length feeding lands where it should.

#define ADAM_CPI        10
#define ADAM_HALF_LPI   12   // half lines per inch
#define ADAM_COLUMNS    95   // 9-1/2" carriage

typedef struct {
    int  column;       // 0 .. ADAM_COLUMNS-1
    int  half_line;    // baseline, in half lines from the top of the current sheet
    int  top_half;     // where the head sits on a fresh sheet
    int  sheet_halves; // half lines per sheet
    bool reverse;      // SO: printing right to left
    bool escape;       // the previous byte was ESC
} AdamHead;

static AdamHead head;

void parser_adam_init(JobState *job) {
    Canvas *c = &job->canvas;
    memset(&head, 0, sizeof(head));
    head.top_half     = ADAM_HALF_LPI / 2;   // 0.5"
    head.sheet_halves = (c->height * ADAM_HALF_LPI) / c->dpi;
    head.half_line    = head.top_half;
    c->line_spacing   = c->dpi / 6;
    c->cur_color      = (RGBColor){0, 0, 0};
}

// Feed the paper by n half lines, starting new sheets as the feed crosses them
static void feed(JobState *job, int n) {
    head.half_line += n;
    while (head.half_line >= head.sheet_halves) {
        job_commit_page(job);
        head.half_line -= head.sheet_halves;
    }
}

static void strike(Canvas *c, char ch) {
    if (ch <= ' ' || ch > '~') return;   // a space moves the head and prints nothing
    const uint8_t *glyph = font5x7_data[ch - ' '];
    // Scale the 5x7 cell to fill most of the 1/10" pitch: 2x2 pixels a dot at 144 DPI
    int scale = c->dpi >= 144 ? c->dpi / 72 : 1;
    int cell_w = c->dpi / ADAM_CPI;
    int x0 = (head.column * c->dpi) / ADAM_CPI + (cell_w - 5 * scale) / 2;
    int y0 = (head.half_line * c->dpi) / ADAM_HALF_LPI - 7 * scale;   // sits on the baseline

    for (int col = 0; col < 5; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 7; row++) {
            if (bits & (1 << row)) {
                canvas_plot_rect(c, x0 + col * scale, y0 + row * scale, scale, scale, c->cur_color);
            }
        }
    }
}

// Move the head in its current direction; SO swaps left and right
static void step(int columns) {
    head.column += head.reverse ? -columns : columns;
    if (head.column < 0) head.column = 0;
    if (head.column > ADAM_COLUMNS - 1) head.column = ADAM_COLUMNS - 1;
}

void parser_adam_byte(JobState *job, uint8_t byte) {
    if (head.escape) {
        head.escape = false;
        return;
    }

    switch (byte) {
    case 0x08: // BS
        step(-1);
        break;
    case 0x0A: // LF
        feed(job, 2);
        break;
    case 0x0B: // VT: half a line
        feed(job, 1);
        break;
    case 0x0C: // FF: the same place on the next sheet
        job_commit_page(job);
        head.half_line = head.top_half;
        break;
    case 0x0D: // CR
        head.column = 0;
        break;
    case 0x0E: // SO: right to left
        head.reverse = true;
        break;
    case 0x0F: // SI: left to right
        head.reverse = false;
        break;
    case 0x1B: // ESC
        head.escape = true;
        break;
    default:
        if (byte >= 0x20 && byte < 0x7F) {
            strike(&job->canvas, (char)byte);
            step(1);
        }
        break;
    }
}
