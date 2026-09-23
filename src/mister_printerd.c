#define _GNU_SOURCE
#include "printer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <signal.h>
#include <time.h>
#include <sys/poll.h>
#include <sys/stat.h>

static volatile bool running = true;

static void sig_handler(int sig) {
    (void)sig;
    running = false;
}

static speed_t baud_to_speed(int baud) {
    switch (baud) {
        case 1200:  return B1200;
        case 2400:  return B2400;
        case 4800:  return B4800;
        case 9600:  return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 115200: return B115200;
        default:    return B9600;
    }
}

static int open_serial_port(const char *path, int baud) {
    if (strcmp(path, "-") == 0 || strcmp(path, "/dev/stdin") == 0) {
        return STDIN_FILENO;
    }

    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        fd = open(path, O_RDONLY);
        if (fd < 0) {
            perror("[printerd] Error opening device/file");
            return -1;
        }
    }

    if (!isatty(fd)) {
        // Regular file or pipe, skip termios configuration
        return fd;
    }

    struct termios tio;
    memset(&tio, 0, sizeof(tio));
    if (tcgetattr(fd, &tio) != 0) {
        perror("[printerd] tcgetattr failed");
        close(fd);
        return -1;
    }

    cfmakeraw(&tio);
    speed_t spd = baud_to_speed(baud);
    cfsetispeed(&tio, spd);
    cfsetospeed(&tio, spd);

    // 8N1 + hardware flow control
    tio.c_cflag &= ~(CSIZE | PARENB | CSTOPB);
    tio.c_cflag |= (CS8 | CLOCAL | CREAD);
#ifdef CRTSCTS
    tio.c_cflag |= CRTSCTS; // RTS/CTS hardware handshake
#endif

    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;

    tcflush(fd, TCIFLUSH);
    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        perror("[printerd] tcsetattr failed");
        close(fd);
        return -1;
    }

    return fd;
}

static void print_usage(const char *prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -d <dev>       Serial device path (default: %s or '-' for stdin)\n", DEFAULT_DEVICE);
    printf("  -b <baud>      Baud rate (default: %d)\n", DEFAULT_BAUD);
    printf("  -m <model>     Printer model: auto | imagewriter | epson | epson-tps | adam | mps803 (default: auto)\n");
    printf("  -c <core>      Active core name (for auto-detect fallback, e.g. Apple-II, ColecoAdam, C64)\n");
    printf("  -o <dir>       Output directory for PDFs (default: %s)\n", DEFAULT_OUTPUT_DIR);
    printf("  -t <sec>       Inactivity timeout in seconds to commit job (default: %d)\n", DEFAULT_TIMEOUT_SEC);
    printf("  -s <size>      Paper size: letter | a4 (default: letter)\n");
    printf("  -r <dpi>       Canvas DPI: 144 | 288 (default: %d)\n", DEFAULT_DPI);
    printf("  -B             Run as background daemon\n");
    printf("  -D             Dump raw serial stream to /tmp/printer_stream.bin (debug)\n");
    printf("  -v             Verbose output\n");
    printf("  -h             Show this help message\n");
}

static PrinterModel get_core_fallback_model(const char *core) {
    if (!core || !*core) return MODEL_EPSON_TPS;
    if (strcasestr(core, "Apple-II") || strcasestr(core, "apple2") ||
        strcasestr(core, "iigs") || strcasestr(core, "Mac")) {
        return MODEL_IMAGEWRITER;
    }
    if (strcasestr(core, "Adam")) {
        return MODEL_ADAM;
    }
    if (strcasestr(core, "C64") || strcasestr(core, "VIC20") ||
        strcasestr(core, "PET") || strcasestr(core, "C16") || strcasestr(core, "Plus4")) {
        return MODEL_MPS803;
    }
    return MODEL_EPSON_TPS;
}

static PrinterModel detect_model_from_stream(const uint8_t *buf, int len) {
    for (int i = 0; i < len; i++) {
        if (buf[i] == 0x1B) { // ESC
            if (i + 1 < len) {
                uint8_t c1 = buf[i + 1];
                // Unambiguous Epson ESC/P markers:
                // ESC @: Initialize / Reset
                // ESC 3 n: n/216" line spacing
                // ESC A n: n/72" line spacing
                // ESC 2 / ESC 0 / ESC 1: standard spacings
                // ESC *: bit image graphics
                // ESC J / ESC j: immediate feed
                if (c1 == '@' || c1 == '3' || c1 == 'A' || c1 == '*' ||
                    c1 == '2' || c1 == '0' || c1 == '1' || c1 == 'J' || c1 == 'j') {
                    return MODEL_EPSON_TPS;
                }

                // Unambiguous Apple ImageWriter markers:
                // ESC c: Software reset
                // ESC T nn: line pitch in 1/144" (followed by 2 ASCII digits)
                // ESC G nnnn / ESC S nnnn: 72/144 DPI graphics (followed by 4 ASCII digits)
                // ESC P / ESC p: 10 cpi pitch
                // ESC >: bidirectional
                // ESC N: 8 lines/inch
                if (c1 == 'c' || c1 == 'T' || c1 == 'G' || c1 == 'g' ||
                    c1 == 'S' || c1 == 's' || c1 == 'P' || c1 == 'p' ||
                    c1 == '>' || c1 == 'N') {
                    return MODEL_IMAGEWRITER;
                }

                // ESC K / L / Y / Z:
                // In Epson, ESC K is graphics followed by two binary length bytes nL, nH.
                // In ImageWriter, ESC K is color select followed by an ASCII digit '0'..'7'.
                if (c1 == 'K' || c1 == 'L' || c1 == 'Y' || c1 == 'Z') {
                    if (i + 2 < len) {
                        uint8_t c2 = buf[i + 2];
                        if (c1 == 'K' && c2 >= '0' && c2 <= '7') {
                            return MODEL_IMAGEWRITER;
                        } else {
                            return MODEL_EPSON_TPS;
                        }
                    }
                }
            }
        } else if (buf[i] == 0x08) { // Commodore PETSCII graphics switch
            return MODEL_MPS803;
        }
    }
    return MODEL_AUTO;
}

static void reset_model_parser(JobState *job) {
    PrinterModel m = (job->model == MODEL_AUTO) ? job->active_model : job->model;
    switch (m) {
        case MODEL_IMAGEWRITER:
            parser_imagewriter_init(job);
            break;
        case MODEL_EPSON:
            parser_escp_init(job, false);
            break;
        case MODEL_EPSON_TPS:
            parser_escp_init(job, true);
            break;
        case MODEL_ADAM:
            parser_adam_init(job);
            break;
        case MODEL_MPS803:
            parser_mps803_init(job);
            break;
        default:
            break;
    }
}

static void dispatch_byte(JobState *job, uint8_t byte) {
    PrinterModel m = (job->model == MODEL_AUTO) ? job->active_model : job->model;
    switch (m) {
        case MODEL_IMAGEWRITER:
            parser_imagewriter_byte(job, byte);
            break;
        case MODEL_EPSON:
        case MODEL_EPSON_TPS:
            parser_escp_byte(job, byte);
            break;
        case MODEL_ADAM:
            parser_adam_byte(job, byte);
            break;
        case MODEL_MPS803:
            parser_mps803_byte(job, byte);
            break;
        default:
            break;
    }
}

int main(int argc, char **argv) {
    PrinterConfig cfg = {
        .device = DEFAULT_DEVICE,
        .baud = DEFAULT_BAUD,
        .model = MODEL_AUTO,
        .core_name = "",
        .output_dir = DEFAULT_OUTPUT_DIR,
        .timeout_sec = DEFAULT_TIMEOUT_SEC,
        .paper_size = PAPER_LETTER,
        .dpi = DEFAULT_DPI,
        .save_png = false,
        .verbose = false,
        .daemon_mode = false
    };

    // Auto-detect core name from /tmp/CORENAME if available
    FILE *fc = fopen("/tmp/CORENAME", "r");
    if (fc) {
        if (fgets(cfg.core_name, sizeof(cfg.core_name), fc)) {
            size_t l = strlen(cfg.core_name);
            while (l > 0 && (cfg.core_name[l-1] == '\r' || cfg.core_name[l-1] == '\n')) {
                cfg.core_name[--l] = '\0';
            }
        }
        fclose(fc);
    }

    // If /media/fat doesn't exist (e.g. testing on host PC/Mac), use ./printers
    struct stat st;
    if (stat("/media/fat", &st) == -1) {
        strcpy(cfg.output_dir, "./printers");
    }

    int opt;
    while ((opt = getopt(argc, argv, "d:b:m:c:o:t:s:r:BvhD")) != -1) {
        switch (opt) {
            case 'd': strncpy(cfg.device, optarg, sizeof(cfg.device) - 1); break;
            case 'b': cfg.baud = atoi(optarg); break;
            case 'm':
                if (strcasecmp(optarg, "auto") == 0) cfg.model = MODEL_AUTO;
                else if (strcasecmp(optarg, "imagewriter") == 0) cfg.model = MODEL_IMAGEWRITER;
                else if (strcasecmp(optarg, "epson") == 0) cfg.model = MODEL_EPSON;
                else if (strcasecmp(optarg, "epson-tps") == 0) cfg.model = MODEL_EPSON_TPS;
                else if (strcasecmp(optarg, "adam") == 0) cfg.model = MODEL_ADAM;
                else if (strcasecmp(optarg, "mps803") == 0) cfg.model = MODEL_MPS803;
                else cfg.model = MODEL_AUTO;
                break;
            case 'c': strncpy(cfg.core_name, optarg, sizeof(cfg.core_name) - 1); break;
            case 'o': strncpy(cfg.output_dir, optarg, sizeof(cfg.output_dir) - 1); break;
            case 't': cfg.timeout_sec = atoi(optarg); break;
            case 's':
                if (strcasecmp(optarg, "a4") == 0) cfg.paper_size = PAPER_A4;
                else cfg.paper_size = PAPER_LETTER;
                break;
            case 'r': cfg.dpi = atoi(optarg); break;
            case 'B': cfg.daemon_mode = true; break;
            case 'D': cfg.dump_stream = true; break;
            case 'v': cfg.verbose = true; break;
            case 'h': print_usage(argv[0]); return 0;
            default: print_usage(argv[0]); return 1;
        }
    }

    if (cfg.daemon_mode) {
#if defined(__APPLE__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        if (daemon(0, 0) != 0) {
            perror("[printerd] Failed to daemonize");
            return 1;
        }
#pragma clang diagnostic pop
#else
        if (daemon(0, 0) != 0) {
            perror("[printerd] Failed to daemonize");
            return 1;
        }
#endif
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    printf("[printerd] Starting mister_printerd (Model: %s, Core: '%s', Baud: %d, Device: %s, Out: %s)\n",
           (cfg.model == MODEL_AUTO) ? "auto" :
           (cfg.model == MODEL_IMAGEWRITER) ? "imagewriter" :
           (cfg.model == MODEL_EPSON) ? "epson" :
           (cfg.model == MODEL_EPSON_TPS) ? "epson-tps" :
           (cfg.model == MODEL_ADAM) ? "adam" : "mps803",
           cfg.core_name, cfg.baud, cfg.device, cfg.output_dir);

    int fd = open_serial_port(cfg.device, cfg.baud);
    if (fd < 0) {
        return 1;
    }

    JobState job;
    memset(&job, 0, sizeof(job));
    job.model = cfg.model;
    job.fallback_model = get_core_fallback_model(cfg.core_name);
    job.active_model = (cfg.model == MODEL_AUTO) ? MODEL_AUTO : cfg.model;
    job.sniff_done = (cfg.model != MODEL_AUTO);
    job.sniff_len = 0;
    job.paper_size = cfg.paper_size;
    job.dpi = cfg.dpi;
    canvas_init(&job.canvas, cfg.dpi, cfg.paper_size);

    if (job.model != MODEL_AUTO) {
        reset_model_parser(&job);
    }

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;

    uint8_t read_buf[512];

    while (running) {
        int poll_res = poll(&pfd, 1, 500); // 500ms timeout
        time_t now = time(NULL);

        if (poll_res > 0 && (pfd.revents & POLLIN)) {
            ssize_t n = read(fd, read_buf, sizeof(read_buf));
            if (n > 0) {
                bool dump_raw = cfg.dump_stream || (access("/tmp/debug_printer_stream", F_OK) == 0);
                if (!job.job_active) {
                    job_start(&job, &cfg);
                    if (job.model != MODEL_AUTO) {
                        reset_model_parser(&job);
                    }
                    if (dump_raw) {
                        // Truncate raw stream file on new job
                        FILE *fraw_init = fopen("/tmp/printer_stream.bin", "wb");
                        if (fraw_init) fclose(fraw_init);
                    }
                }
                job.last_data_time = (long)now;

                if (dump_raw) {
                    FILE *fraw = fopen("/tmp/printer_stream.bin", "ab");
                    if (fraw) {
                        fwrite(read_buf, 1, n, fraw);
                        fclose(fraw);
                    }
                }

                for (ssize_t i = 0; i < n; i++) {
                    uint8_t b = read_buf[i];
                    if (job.model == MODEL_AUTO && !job.sniff_done) {
                        if (job.sniff_len < (int)sizeof(job.sniff_buf)) {
                            job.sniff_buf[job.sniff_len++] = b;
                        }
                        PrinterModel detected = detect_model_from_stream(job.sniff_buf, job.sniff_len);
                        if (detected != MODEL_AUTO || job.sniff_len >= 128) {
                            job.active_model = (detected != MODEL_AUTO) ? detected : job.fallback_model;
                            job.sniff_done = true;
                            if (cfg.verbose) {
                                printf("[printerd] Auto-detected printer model: %d (stream: %d, fallback: %d)\n",
                                       job.active_model, detected, job.fallback_model);
                            }
                            reset_model_parser(&job);
                            for (int j = 0; j < job.sniff_len; j++) {
                                dispatch_byte(&job, job.sniff_buf[j]);
                            }
                        }
                    } else {
                        dispatch_byte(&job, b);
                    }
                }
            } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                if (cfg.verbose) perror("[printerd] read error");
                break;
            } else if (n == 0) {
                if (!isatty(fd) || strcmp(cfg.device, "-") == 0 || strcmp(cfg.device, "/dev/stdin") == 0) {
                    if (cfg.verbose) printf("[printerd] End of input file/pipe reached.\n");
                    break;
                }
            }
        }

        // Inactivity timeout check: flush active print job to PDF
        if (job.job_active) {
            if ((long)now - job.last_data_time >= cfg.timeout_sec) {
                if (cfg.verbose) printf("[printerd] Inactivity timeout reached (%ds), finalizing job...\n", cfg.timeout_sec);
                if (job.model == MODEL_AUTO && !job.sniff_done && job.sniff_len > 0) {
                    job.active_model = job.fallback_model;
                    job.sniff_done = true;
                    reset_model_parser(&job);
                    for (int j = 0; j < job.sniff_len; j++) {
                        dispatch_byte(&job, job.sniff_buf[j]);
                    }
                }
                job_finalize(&job, &cfg);
                if (job.model == MODEL_AUTO) {
                    job.active_model = MODEL_AUTO;
                    job.sniff_done = false;
                    job.sniff_len = 0;
                } else {
                    reset_model_parser(&job);
                }
            }
        }
    }

    // Flush any pending job on termination
    if (job.job_active) {
        if (job.model == MODEL_AUTO && !job.sniff_done && job.sniff_len > 0) {
            job.active_model = job.fallback_model;
            job.sniff_done = true;
            reset_model_parser(&job);
            for (int j = 0; j < job.sniff_len; j++) {
                dispatch_byte(&job, job.sniff_buf[j]);
            }
        }
        job_finalize(&job, &cfg);
    }

    canvas_free(&job.canvas);
    if (fd != STDIN_FILENO) {
        close(fd);
    }

    printf("[printerd] Shutdown complete.\n");
    return 0;
}
