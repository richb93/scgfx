#include "scgfx_internal.h"

void usage(void) {
    fprintf(stderr,
        "scgfx - a tool for encoding/decoding Roland Sound Canvas graphics data\n"
        "Created by richb93 with the help of ChatGPT.\n"
        "v%s - %s\n\n"
        "Usage:\n\n"
        "  To encode a single image or animation to a new MIDI file:\n"
        "    scgfx encode [-v|-vv] [-fps N] [-delta] -o output.mid frame1.pbm ...\n\n"
        "  To merge graphics as an extra track with an existing MIDI file:\n"
        "    scgfx encode [-v|-vv] [-fps N] [-delta] -m input.mid -o output.mid frame*.pbm ...\n\n"
        "  To interleave graphics into a Format 0 MIDI file:\n"
        "    scgfx encode -interleave [-fps N] [-delta] -m input.mid -o output.mid frame*.pbm ...\n\n"
        "  To decode from a MIDI file:\n"
        "    scgfx decode [-format pbm|ascii] [-grid ticks] [--extract-pages] input.mid\n"
        "      --extract-pages also writes SC-88 page RAM snapshots as pageNN_XXXX.pbm\n\n"
        "  To analyse graphics/display timing in a MIDI file:\n"
        "    scgfx analyse [-dump] [-log file] input.mid\n\n"
        "  To compile a project file into MIDI:\n"
        "    scgfx compile [-v|-vv|-verbose] project.scg\n\n"
        "Script format (.scg):\n\n"
        "  Project settings:\n"
        "    output \"out.mid\"                 Set output MIDI filename\n"
        "    input \"song.mid\"                 Optional Format 0 input MIDI\n"
        "    fps N                           Set project animation FPS\n"
        "    delta on|off                    Enable/disable SC-88 delta writes\n"
        "    songlength TICKS                Optional end tick for looped animations\n\n"
        "  Resources:\n"
        "    page N \"image.pbm\"             Load one image into SC-88 page N\n"
        "    page N \"frames/*.pbm\"          Load wildcard matches into pages N-10\n"
        "                                    Fails if too many files match\n"
        "    animation NAME \"frames/*.pbm\"  Define named image animation\n"
        "    pageanim NAME 1-10              Define named page animation\n\n"
        "  Timeline:\n"
        "    at TICK text \"message\"         Display up to 32 ASCII chars\n"
        "    at TICK page N                  Display page 0-10\n"
        "    at TICK time N                  Display time raw value 0-15\n"
        "                                    also accepts seconds/ms, e.g. 2.88s or 1500ms\n"
        "                                    1 unit = 0.48s; default 6 = 2.88s\n"
        "    at TICK image \"file.pbm\"       Immediate image write\n"
        "    at TICK animate pages 1-6       Page animation using project FPS\n"
        "    at TICK animate images \"frames/*.pbm\"\n"
        "    at TICK play NAME [fps N] [repeat N|loop]\n\n"
        "Input data format for encode:\n\n"
        "  .txt ASCII art: 16 lines of 16 chars, # = on, . = off\n"
        "  PBM P1 or P4: 16x16 monochrome\n\n"
        "Options:\n"
        "  -delta sends only changed framebuffer byte ranges after the first frame.\n"
        "         This is intended for SC-88 or later; it is not SC-55 compatible.\n\n"
        "FFmpeg PBM creation example:\n"
        "  ffmpeg -i input.mp4 -vf \"fps=10,scale=16:16:flags=lanczos,format=monow\" frame%%04d.pbm\n\n",
        SCGFX_VERSION, SCGFX_DATE);
}

void write_be16(FILE *f, uint16_t v) { fputc((v >> 8) & 0xff, f); fputc(v & 0xff, f); }

void write_be32(FILE *f, uint32_t v) { fputc((v >> 24) & 0xff, f); fputc((v >> 16) & 0xff, f); fputc((v >> 8) & 0xff, f); fputc(v & 0xff, f); }

uint16_t read_be16(const uint8_t *p) { return ((uint16_t)p[0] << 8) | p[1]; }

uint32_t read_be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

void write_varlen(FILE *f, uint32_t v) {
    uint8_t buf[5]; int n = 0;
    buf[n++] = v & 0x7f;
    while ((v >>= 7)) buf[n++] = 0x80 | (v & 0x7f);
    while (n--) fputc(buf[n], f);
}

int encode_varlen_to_buf(uint32_t v, uint8_t *out) {
    uint8_t tmp[5]; int n = 0, written = 0;
    tmp[n++] = v & 0x7f;
    while ((v >>= 7)) tmp[n++] = 0x80 | (v & 0x7f);
    while (n--) out[written++] = tmp[n];
    return written;
}

int read_varlen_from_buf(const uint8_t *data, size_t size, size_t *pos, uint32_t *value) {
    uint32_t v = 0; int count = 0;
    while (*pos < size) {
        uint8_t c = data[(*pos)++];
        v = (v << 7) | (c & 0x7f);
        count++;
        if (!(c & 0x80)) { *value = v; return 1; }
        if (count >= 4) return 0;
    }
    return 0;
}

int read_file(const char *name, uint8_t **data, size_t *size) {
    FILE *f = fopen(name, "rb"); long len;
    if (!f) { perror(name); return 0; }
    fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return 0; }
    *data = (uint8_t *)malloc((size_t)len);
    if (!*data) { fclose(f); return 0; }
    if (fread(*data, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(*data); return 0; }
    fclose(f); *size = (size_t)len; return 1;
}

int pbm_next_token(FILE *f, char *buf, int buflen) {
    int c, i = 0;
    do {
        c = fgetc(f);
        if (c == '#') while (c != '\n' && c != EOF) c = fgetc(f);
    } while (c != EOF && isspace(c));
    if (c == EOF) return 0;
    while (c != EOF && !isspace(c)) { if (i < buflen - 1) buf[i++] = (char)c; c = fgetc(f); }
    buf[i] = 0; return 1;
}

char *trim_left(char *s) {
    while (*s && isspace((unsigned char)*s))
        s++;
    return s;
}

void trim_right(char *s) {
    size_t len = strlen(s);

    while (len > 0 && isspace((unsigned char)s[len - 1]))
        s[--len] = 0;
}

int parse_quoted_string(const char *p, char *out, size_t outlen) {
    size_t n = 0;

    while (*p && isspace((unsigned char)*p))
        p++;

    if (*p != '"')
        return 0;

    p++;

    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            p++;

            if (*p == 'n') {
                if (n + 1 < outlen)
                    out[n++] = '\n';
            } else if (*p == 't') {
                if (n + 1 < outlen)
                    out[n++] = '\t';
            } else {
                if (n + 1 < outlen)
                    out[n++] = *p;
            }

            p++;
        } else {
            if (n + 1 < outlen)
                out[n++] = *p;
            p++;
        }
    }

    if (*p != '"')
        return 0;

    out[n] = 0;
    return 1;
}

int parse_on_off(const char *s, int *value) {
    if (strcmp(s, "on") == 0 || strcmp(s, "1") == 0 || strcmp(s, "true") == 0) {
        *value = 1;
        return 1;
    }

    if (strcmp(s, "off") == 0 || strcmp(s, "0") == 0 || strcmp(s, "false") == 0) {
        *value = 0;
        return 1;
    }

    return 0;
}


int parse_display_time_value(const char *p, int *value_out) {
    /*
        Roland SC-88 Display Time register:

            10 20 01 = 00-0F
            06 = 2.88 seconds
            0F = 7.20 seconds

        Therefore each unit is 0.48 seconds.

        Accepted forms:
            6       raw Roland value, 0-15
            2.88s   seconds, rounded to nearest raw value
            2.88sec seconds, rounded to nearest raw value
            1500ms  milliseconds, rounded to nearest raw value

        Bare numbers remain raw values for backwards compatibility.
    */
    char *endp = NULL;
    double value;
    double seconds;
    int raw;

    p = trim_left((char *)p);

    if (*p == 0)
        return 0;

    value = strtod(p, &endp);

    if (endp == p)
        return 0;

    while (*endp && isspace((unsigned char)*endp))
        endp++;

    if (*endp == 0) {
        /*
            Backwards-compatible raw register value.
            Reject decimals without a unit to avoid ambiguity.
        */
        if (strchr(p, '.') != NULL)
            return 0;

        raw = (int)value;
    } else if (strcmp(endp, "s") == 0 ||
               strcmp(endp, "sec") == 0 ||
               strcmp(endp, "secs") == 0 ||
               strcmp(endp, "second") == 0 ||
               strcmp(endp, "seconds") == 0) {
        seconds = value;
        raw = (int)((seconds / 0.48) + 0.5);
    } else if (strcmp(endp, "ms") == 0 ||
               strcmp(endp, "msec") == 0 ||
               strcmp(endp, "msecs") == 0 ||
               strcmp(endp, "millisecond") == 0 ||
               strcmp(endp, "milliseconds") == 0) {
        seconds = value / 1000.0;
        raw = (int)((seconds / 0.48) + 0.5);
    } else {
        return 0;
    }

    if (raw < 0 || raw > 15)
        return 0;

    *value_out = raw;
    return 1;
}


int parse_optional_quoted_or_plain(const char *p, char *out, size_t outlen) {
    while (*p && isspace((unsigned char)*p))
        p++;

    if (*p == '"')
        return parse_quoted_string(p, out, outlen);

    size_t n = 0;
    while (*p && !isspace((unsigned char)*p)) {
        if (n + 1 < outlen)
            out[n++] = *p;
        p++;
    }

    out[n] = 0;
    return n > 0;
}

