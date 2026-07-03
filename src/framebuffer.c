#include "scgfx_internal.h"

int load_txt(const char *filename, uint8_t pixels[16][16]) {
    FILE *f = fopen(filename, "r"); char line[128];
    if (!f) { perror(filename); return 0; }
    memset(pixels, 0, 256);
    for (int y = 0; y < 16; y++) {
        if (!fgets(line, sizeof(line), f)) { fprintf(stderr, "%s: expected 16 lines\n", filename); fclose(f); return 0; }
        size_t len = strcspn(line, "\r\n");
        if (len != 16) { fprintf(stderr, "%s: line %d is %zu chars, expected 16\n", filename, y + 1, len); fclose(f); return 0; }
        for (int x = 0; x < 16; x++) {
            if (line[x] == '#') pixels[x][y] = 1;
            else if (line[x] == '.') pixels[x][y] = 0;
            else { fprintf(stderr, "%s: invalid character '%c'\n", filename, line[x]); fclose(f); return 0; }
        }
    }
    fclose(f); return 1;
}

int load_pbm_p1(const char *filename, uint8_t pixels[16][16]) {
    FILE *f = fopen(filename, "r"); char tok[64];
    if (!f) { perror(filename); return 0; }
    memset(pixels, 0, 256);
    if (!pbm_next_token(f, tok, sizeof(tok)) || strcmp(tok, "P1") != 0) { fclose(f); return 0; }
    if (!pbm_next_token(f, tok, sizeof(tok)) || atoi(tok) != 16 || !pbm_next_token(f, tok, sizeof(tok)) || atoi(tok) != 16) { fprintf(stderr, "%s: PBM must be 16x16\n", filename); fclose(f); return 0; }
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
        if (!pbm_next_token(f, tok, sizeof(tok))) { fprintf(stderr, "%s: not enough PBM data\n", filename); fclose(f); return 0; }
        pixels[x][y] = atoi(tok) ? 1 : 0;
    }
    fclose(f); return 1;
}

int load_pbm_p4(const char *filename, uint8_t pixels[16][16]) {
    FILE *f = fopen(filename, "rb"); char tok[64];
    if (!f) { perror(filename); return 0; }
    memset(pixels, 0, 256);
    if (!pbm_next_token(f, tok, sizeof(tok)) || strcmp(tok, "P4") != 0) { fclose(f); return 0; }
    if (!pbm_next_token(f, tok, sizeof(tok)) || atoi(tok) != 16 || !pbm_next_token(f, tok, sizeof(tok)) || atoi(tok) != 16) { fprintf(stderr, "%s: PBM must be 16x16\n", filename); fclose(f); return 0; }
    for (int y = 0; y < 16; y++) {
        uint8_t row[2];
        if (fread(row, 1, 2, f) != 2) { fprintf(stderr, "%s: not enough P4 data\n", filename); fclose(f); return 0; }
        for (int x = 0; x < 16; x++) pixels[x][y] = (row[x / 8] >> (7 - (x % 8))) & 1;
    }
    fclose(f); return 1;
}

int load_frame(const char *filename, uint8_t pixels[16][16]) {
    FILE *f = fopen(filename, "rb"); char magic[2];
    if (!f) { perror(filename); return 0; }
    if (fread(magic, 1, 2, f) != 2) { fclose(f); return 0; }
    fclose(f);
    if (magic[0] == 'P' && magic[1] == '1') return load_pbm_p1(filename, pixels);
    if (magic[0] == 'P' && magic[1] == '4') return load_pbm_p4(filename, pixels);
    return load_txt(filename, pixels);
}

void build_framebuffer(uint8_t pixels[16][16], uint8_t fb[64]) {
    memset(fb, 0, 64);
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
        if (!pixels[x][y]) continue;
        int group = x / 5, col = x % 5;
        fb[group * 16 + y] |= (uint8_t)(1 << (4 - col));
    }
}

void framebuffer_to_pixels(const uint8_t fb[64], uint8_t pixels[16][16]) {
    memset(pixels, 0, 256);
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
        int group = x / 5, col = x % 5;
        pixels[x][y] = (fb[group * 16 + y] >> (4 - col)) & 1;
    }
}

char *make_output_dir(const char *input) {
    const char *base = strrchr(input, '/');
#ifdef _WIN32
    const char *base2 = strrchr(input, '\\');
    if (!base || (base2 && base2 > base)) base = base2;
#endif
    base = base ? base + 1 : input;
    char *out = (char *)malloc(strlen(base) + 1);
    if (!out) return NULL;
    strcpy(out, base);
    char *dot = strrchr(out, '.'); if (dot) *dot = 0;
    if (out[0] == 0) strcpy(out, "frames");
    return out;
}

int write_pbm(const char *dir, int frame_no, const uint8_t pixels[16][16]) {
    char path[1024]; snprintf(path, sizeof(path), "%s/frame%04d.pbm", dir, frame_no);
    FILE *f = fopen(path, "w"); if (!f) { perror(path); return 0; }
    fprintf(f, "P1\n16 16\n");
    for (int y = 0; y < 16; y++) { for (int x = 0; x < 16; x++) fprintf(f, "%d ", pixels[x][y] ? 1 : 0); fprintf(f, "\n"); }
    fclose(f); return 1;
}

int write_ascii(const char *dir, int frame_no, const uint8_t pixels[16][16]) {
    char path[1024]; snprintf(path, sizeof(path), "%s/frame%04d.txt", dir, frame_no);
    FILE *f = fopen(path, "w"); if (!f) { perror(path); return 0; }
    for (int y = 0; y < 16; y++) { for (int x = 0; x < 16; x++) fputc(pixels[x][y] ? '#' : '.', f); fputc('\n', f); }
    fclose(f); return 1;
}

int output_frame_file(const char *outdir,
                             int output_ascii,
                             int frame_no,
                             const uint8_t fb[64]) {
    uint8_t pixels[16][16];

    framebuffer_to_pixels(fb, pixels);

    return output_ascii ?
        write_ascii(outdir, frame_no, pixels) :
        write_pbm(outdir, frame_no, pixels);
}

