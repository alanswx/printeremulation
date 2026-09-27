#include "printer.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static const RGBColor IW_COLORS[8] = {
    {0, 0, 0},        // 0: Black
    {255, 220, 0},    // 1: Yellow
    {227, 27, 35},    // 2: Red / Magenta
    {0, 128, 255},    // 3: Blue / Cyan
    {255, 128, 0},    // 4: Orange (Yellow + Red)
    {0, 176, 80},     // 5: Green (Yellow + Blue)
    {112, 48, 160},   // 6: Purple (Red + Blue)
    {0, 0, 0}         // 7: Default Black
};

// Minimal 5x7 dot-matrix font for ASCII 32..126
#include "font5x7.h"

typedef enum {
    IW_STATE_TEXT,
    IW_STATE_TAB,
    IW_STATE_TAB_NUM,
    IW_STATE_ESC,
    IW_STATE_LINE_PITCH_1,
    IW_STATE_LINE_PITCH_2,
    IW_STATE_COLOR,
    IW_STATE_HORIZ_POS,
    IW_STATE_GRAPHICS_LEN,
    IW_STATE_GRAPHICS_DATA,
    IW_STATE_PARAMS,     // collecting fixed-length parameters for iw.p_cmd
    IW_STATE_SKIP_DATA   // discarding iw.skip_count data bytes (unsupported graphics)
} IWState;

static struct {
    IWState state;
    char g_cmd;        // 'G' (72 DPI) or 'S' (144 DPI) or 'g'
    int g_cols_expected;
    int g_cols_read;
    char g_len_str[5];
    int g_len_idx;
    int pitch_val;
    int dpi_mode;
    int start_col;
    char p_cmd;        // ESC command whose parameters are being collected
    char p_buf[8];
    int p_len;
    int p_needed;
    long skip_count;
    int col_valid;     // start_col matches head_x (cleared when text moves the head)
} iw;

void parser_imagewriter_init(JobState *job) {
    (void)job;
    iw.state = IW_STATE_TEXT;
    iw.g_cols_expected = 0;
    iw.g_cols_read = 0;
    iw.g_len_idx = 0;
    iw.dpi_mode = 72;
    iw.start_col = 0;
    iw.col_valid = 1;
    job->canvas.line_spacing = (job->canvas.dpi * 24) / 144; // 24/144" = 1/6"
    job->canvas.cur_color = IW_COLORS[0];
}

static void draw_char(Canvas *c, char ch) {
    if (ch < 32 || ch > 126) ch = ' ';
    int idx = ch - 32;
    int scale = (c->dpi >= 144) ? (c->dpi / 72) : 1;

    for (int col = 0; col < 5; col++) {
        uint8_t bits = font5x7_data[idx][col];
        for (int row = 0; row < 7; row++) {
            if (bits & (1 << row)) {
                int px = c->head_x + (col * scale);
                int py = c->head_y + (row * scale);
                canvas_plot_dot(c, px, py, c->cur_color);
                if (scale > 1) {
                    canvas_plot_dot(c, px + 1, py, c->cur_color);
                    canvas_plot_dot(c, px, py + 1, c->cur_color);
                    canvas_plot_dot(c, px + 1, py + 1, c->cur_color);
                }
            }
        }
    }
    c->head_x += 6 * scale; // 5 dots + 1 dot space
    iw.col_valid = 0;
}

static void advance_tab(Canvas *c) {
    int scale = (c->dpi >= 144) ? (c->dpi / 72) : 1;
    int char_width = 6 * scale;
    int tab_width = char_width * 8; // standard 8-character tab stop
    int margin_left = (int)(0.5f * c->dpi);
    int rel_x = c->head_x - margin_left;
    if (rel_x < 0) rel_x = 0;
    c->head_x = margin_left + ((rel_x / tab_width) + 1) * tab_width;
    iw.col_valid = 0;
}

// Draw one 8-pin graphics column at the current graphics position
static void draw_graphics_column(Canvas *c, uint8_t byte) {
    int margin_left = (int)(0.5f * c->dpi);
    int unit = (iw.dpi_mode > 0) ? iw.dpi_mode : 72;
    int v_scale = c->dpi / 72; // 72 DPI vertical pin spacing = 2 px at 144 DPI
    if (v_scale < 1) v_scale = 1;

    int col_x = margin_left + (int)(((long)(iw.start_col + iw.g_cols_read) * c->dpi) / unit);
    int col_next = margin_left + (int)(((long)(iw.start_col + iw.g_cols_read + 1) * c->dpi) / unit);
    int dot_w = col_next - col_x;
    if (dot_w < 1) dot_w = 1;

    // Bit 0 is top dot (Pin 1), Bit 7 is bottom dot (Pin 8)
    for (int pin = 0; pin < 8; pin++) {
        if (byte & (1 << pin)) {
            int py = c->head_y + (pin * v_scale);
            for (int dx = 0; dx < dot_w; dx++) {
                for (int dy = 0; dy < v_scale; dy++) {
                    canvas_plot_dot(c, col_x + dx, py + dy, c->cur_color);
                }
            }
        }
    }
    c->head_x = col_next;
    iw.g_cols_read++;
}

// Decimal parameter field; the ImageWriter treats leading spaces as zeros
static int param_num(const char *p, int len) {
    int v = 0;
    for (int i = 0; i < len; i++) {
        char ch = (p[i] == ' ') ? '0' : p[i];
        if (!isdigit((unsigned char)ch)) return 0;
        v = v * 10 + (ch - '0');
    }
    return v;
}

// Graphics columns are placed from start_col (in graphics-mode units). After text
// has moved the head, recompute start_col from head_x before the next graphics.
static void sync_graphics_col(Canvas *c) {
    if (iw.col_valid) return;
    int margin_left = (int)(0.5f * c->dpi);
    int unit = (iw.dpi_mode > 0) ? iw.dpi_mode : 72;
    iw.start_col = (int)(((long)(c->head_x - margin_left) * unit + c->dpi / 2) / c->dpi);
    if (iw.start_col < 0) iw.start_col = 0;
    iw.col_valid = 1;
}

// Finish a graphics run: later graphics continue after its last column
static void end_graphics(void) {
    iw.start_col += iw.g_cols_read;
    iw.g_cols_read = 0;
}

static void begin_params(char cmd, int needed) {
    iw.p_cmd = cmd;
    iw.p_len = 0;
    iw.p_needed = needed;
    iw.state = IW_STATE_PARAMS;
}

void parser_imagewriter_byte(JobState *job, uint8_t byte) {
    Canvas *c = &job->canvas;
    int margin_left = (int)(0.5f * c->dpi);
    int margin_bottom = c->height - (int)(0.5f * c->dpi);

    switch (iw.state) {
        case IW_STATE_TEXT:
            if (byte == 0x1B) { // ESC
                iw.state = IW_STATE_ESC;
            } else if (byte == 0x09) { // HT / Ctrl-I (Tab or Super Serial Card command)
                iw.state = IW_STATE_TAB;
            } else if (byte == 0x0D) { // CR
                c->head_x = margin_left;
                iw.start_col = 0;
                iw.col_valid = 1;
            } else if (byte == 0x0A) { // LF
                c->head_y += c->line_spacing;
                c->head_x = margin_left;
                iw.start_col = 0;
                iw.col_valid = 1;
                if (c->head_y >= margin_bottom) {
                    job_commit_page(job);
                }
            } else if (byte == 0x0C) { // Form Feed
                job_commit_page(job);
            } else if (byte >= 32 && byte <= 126) {
                draw_char(c, (char)byte);
            }
            break;

        case IW_STATE_TAB:
            if (byte == 'Z' || byte == 'z') {
                // Super Serial Card reset: <Ctrl-I> Z -> swallow
                iw.state = IW_STATE_TEXT;
            } else if (byte == 'N' || byte == 'n') {
                // Super Serial Card line length/no LF: <Ctrl-I> N -> swallow
                iw.state = IW_STATE_TEXT;
            } else if (isdigit(byte)) {
                // Super Serial Card parameter: <Ctrl-I> 80N or 0N
                iw.state = IW_STATE_TAB_NUM;
            } else {
                // Not an SSC card command -> treat as real Tab
                advance_tab(c);
                iw.state = IW_STATE_TEXT;
                parser_imagewriter_byte(job, byte);
            }
            break;

        case IW_STATE_TAB_NUM:
            if (isdigit(byte)) {
                // Continue consuming digits (e.g. "80", "132")
            } else if (byte == 'N' || byte == 'n') {
                // End of SSC command: <Ctrl-I> 80N -> swallow
                iw.state = IW_STATE_TEXT;
            } else {
                // Unrecognized sequence after digits -> advance tab and re-process byte
                advance_tab(c);
                iw.state = IW_STATE_TEXT;
                parser_imagewriter_byte(job, byte);
            }
            break;

        case IW_STATE_ESC:
            if (byte == 'T') {
                iw.state = IW_STATE_LINE_PITCH_1;
            } else if (byte == 'K') {
                iw.state = IW_STATE_COLOR;
            } else if (byte == 'F') { // Absolute horizontal position: ESC F nnnn
                iw.g_len_idx = 0;
                iw.state = IW_STATE_HORIZ_POS;
            } else if (byte == 'f') { // Forward half-line feed (1/12")
                c->head_y += c->line_spacing / 2;
                iw.state = IW_STATE_TEXT;
            } else if (byte == 'r') { // Reverse line feed (1/6")
                c->head_y -= c->line_spacing;
                int margin_top = (int)(0.5f * c->dpi);
                if (c->head_y < margin_top) c->head_y = margin_top;
                iw.state = IW_STATE_TEXT;
            } else if (byte == 'G' || byte == 'S' || byte == 'g') {
                iw.g_cmd = (char)byte;
                iw.g_len_idx = 0;
                iw.state = IW_STATE_GRAPHICS_LEN;
            } else if (byte == 'n') {
                iw.dpi_mode = 72;
                iw.state = IW_STATE_TEXT;
            } else if (byte == 'N') {
                iw.dpi_mode = 80;
                iw.state = IW_STATE_TEXT;
            } else if (byte == 'E') {
                iw.dpi_mode = 96;
                iw.state = IW_STATE_TEXT;
            } else if (byte == 'e') {
                iw.dpi_mode = 107;
                iw.state = IW_STATE_TEXT;
            } else if (byte == 'q') {
                iw.dpi_mode = 120;
                iw.state = IW_STATE_TEXT;
            } else if (byte == 'Q') {
                iw.dpi_mode = 136;
                iw.state = IW_STATE_TEXT;
            } else if (byte == 'p') {
                iw.dpi_mode = 144;
                iw.state = IW_STATE_TEXT;
            } else if (byte == 'P') {
                iw.dpi_mode = 160;
                iw.state = IW_STATE_TEXT;
            } else if (byte == 'A') { // 1/6"
                c->line_spacing = c->dpi / 6;
                iw.state = IW_STATE_TEXT;
            } else if (byte == 'B') { // 1/8"
                c->line_spacing = c->dpi / 8;
                iw.state = IW_STATE_TEXT;
            } else if (byte == 'V') { // Repeat dot column c nnnn times: ESC V nnnn c
                begin_params('V', 5);
            } else if (byte == 'R') { // Repeat character c nnn times: ESC R nnn c
                begin_params('R', 4);
            } else if (byte == 'C') { // Hi-res graphics, nnnn*3 data bytes (LQ): skipped
                begin_params('C', 4);
            } else if (byte == 'U') { // Hi-res repeat column (LQ): ESC U nnnn abc, skipped
                begin_params('U', 7);
            } else if (byte == 'H' || byte == 'h') { // Page length / hi-res head position
                begin_params((char)byte, 4);
            } else if (byte == 'L' || byte == 'u') { // Left margin / add tab stop: nnn
                begin_params((char)byte, 3);
            } else if (byte == 'D' || byte == 'Z') { // Soft switches: two bitmask bytes
                begin_params((char)byte, 2);
            } else if (byte == '=' || byte == '@' || byte == 'a' || byte == 'l' ||
                       byte == 's' || byte == 't') { // One-byte parameter commands
                begin_params((char)byte, 1);
            } else if (byte == 'c' || byte == '?') { // Reset
                c->line_spacing = c->dpi / 6;
                c->cur_color = IW_COLORS[0];
                iw.dpi_mode = 72;
                iw.state = IW_STATE_TEXT;
            } else {
                // Other escape sequence without parameters ('>', '<', '!', '"', 'X', 'Y', etc.)
                iw.state = IW_STATE_TEXT;
            }
            break;

        case IW_STATE_LINE_PITCH_1:
            if (isdigit(byte)) {
                iw.pitch_val = (byte - '0') * 10;
                iw.state = IW_STATE_LINE_PITCH_2;
            } else {
                iw.state = IW_STATE_TEXT;
            }
            break;

        case IW_STATE_LINE_PITCH_2:
            if (isdigit(byte)) {
                iw.pitch_val += (byte - '0');
                // Pitch is in 1/144" units
                c->line_spacing = (job->canvas.dpi * iw.pitch_val) / 144;
            }
            iw.state = IW_STATE_TEXT;
            break;

        case IW_STATE_COLOR: {
            int col = (byte >= '0' && byte <= '6') ? (byte - '0') : 0;
            c->cur_color = IW_COLORS[col];
            iw.state = IW_STATE_TEXT;
            break;
        }

        case IW_STATE_HORIZ_POS: {
            char ch = (char)byte;
            if (ch == ' ') ch = '0';
            if (isdigit((unsigned char)ch)) {
                iw.g_len_str[iw.g_len_idx++] = ch;
                if (iw.g_len_idx == 4) {
                    iw.g_len_str[4] = '\0';
                    iw.start_col = atoi(iw.g_len_str);
                    iw.col_valid = 1;
                    int unit = (iw.dpi_mode > 0) ? iw.dpi_mode : 72;
                    c->head_x = margin_left + (int)(((long)iw.start_col * c->dpi) / unit);
                    iw.state = IW_STATE_TEXT;
                }
            } else {
                iw.state = IW_STATE_TEXT;
            }
            break;
        }

        case IW_STATE_GRAPHICS_LEN: {
            char ch = (char)byte;
            if (ch == ' ') ch = '0';
            int target_len = (iw.g_cmd == 'g') ? 3 : 4;
            if (isdigit((unsigned char)ch)) {
                iw.g_len_str[iw.g_len_idx++] = ch;
                if (iw.g_len_idx == target_len) {
                    iw.g_len_str[target_len] = '\0';
                    iw.g_cols_expected = atoi(iw.g_len_str);
                    if (iw.g_cmd == 'g') iw.g_cols_expected *= 8; // ESC g nnn = nnn*8 bytes
                    iw.g_cols_read = 0;
                    sync_graphics_col(c);
                    if (iw.g_cols_expected > 0) {
                        iw.state = IW_STATE_GRAPHICS_DATA;
                    } else {
                        iw.state = IW_STATE_TEXT;
                    }
                }
            } else {
                iw.state = IW_STATE_TEXT;
            }
            break;
        }

        case IW_STATE_GRAPHICS_DATA:
            draw_graphics_column(c, byte);
            if (iw.g_cols_read >= iw.g_cols_expected) {
                end_graphics();
                iw.state = IW_STATE_TEXT;
            }
            break;

        case IW_STATE_PARAMS:
            iw.p_buf[iw.p_len++] = (char)byte;
            if (iw.p_len < iw.p_needed) break;
            iw.state = IW_STATE_TEXT;
            if (iw.p_cmd == 'V') {
                int n = param_num(iw.p_buf, 4);
                sync_graphics_col(c);
                iw.g_cols_read = 0;
                for (int i = 0; i < n; i++)
                    draw_graphics_column(c, (uint8_t)iw.p_buf[4]);
                end_graphics();
            } else if (iw.p_cmd == 'R') {
                int n = param_num(iw.p_buf, 3);
                for (int i = 0; i < n; i++)
                    draw_char(c, iw.p_buf[3]);
            } else if (iw.p_cmd == 'C') {
                iw.skip_count = (long)param_num(iw.p_buf, 4) * 3;
                if (iw.skip_count > 0) iw.state = IW_STATE_SKIP_DATA;
            }
            break;

        case IW_STATE_SKIP_DATA:
            if (--iw.skip_count <= 0) iw.state = IW_STATE_TEXT;
            break;
    }
}

