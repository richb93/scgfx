#include "scgfx_internal.h"

void make_scgfx_metadata_text(char *buf, size_t buflen,
                                     int frames, int fps,
                                     uint32_t grid,
                                     int delta_mode) {
    snprintf(buf, buflen,
             "SCGFX:frames=%d;fps=%d;grid=%u;delta=%d;version=%s",
             frames, fps, grid, delta_mode ? 1 : 0, SCGFX_VERSION);
}

void emit_meta_text_event(FILE *f, uint32_t delay, const char *text) {
    size_t len = strlen(text);

    write_varlen(f, delay);
    fputc(0xFF, f);
    fputc(0x01, f);
    write_varlen(f, (uint32_t)len);
    fwrite(text, 1, len, f);
}

uint8_t *make_meta_text_event_data(const char *text, uint32_t *out_len) {
    uint8_t lenbuf[5];
    size_t text_len = strlen(text);
    int lenbytes = encode_varlen_to_buf((uint32_t)text_len, lenbuf);
    uint32_t total = 2 + (uint32_t)lenbytes + (uint32_t)text_len;

    uint8_t *ev = (uint8_t *)malloc(total);
    if (!ev)
        return NULL;

    int p = 0;
    ev[p++] = 0xFF;
    ev[p++] = 0x01;
    memcpy(ev + p, lenbuf, lenbytes);
    p += lenbytes;
    memcpy(ev + p, text, text_len);

    *out_len = total;
    return ev;
}

void parse_scgfx_metadata_text(const char *text, size_t len,
                                      ScgfxMetadata *meta) {
    const char prefix[] = "SCGFX:";

    if (len < strlen(prefix))
        return;

    if (memcmp(text, prefix, strlen(prefix)) != 0)
        return;

    meta->found = 1;

    char *tmp = (char *)malloc(len + 1);
    if (!tmp)
        return;

    memcpy(tmp, text, len);
    tmp[len] = 0;

    char *p = tmp + strlen(prefix);

    while (*p) {
        char *next = strchr(p, ';');
        if (next)
            *next = 0;

        if (strncmp(p, "frames=", 7) == 0)
            meta->frames = (uint32_t)atoi(p + 7);
        else if (strncmp(p, "fps=", 4) == 0)
            meta->fps = (uint32_t)atoi(p + 4);
        else if (strncmp(p, "grid=", 5) == 0)
            meta->grid = (uint32_t)atoi(p + 5);
        else if (strncmp(p, "delta=", 6) == 0)
            meta->delta = atoi(p + 6);

        if (!next)
            break;

        p = next + 1;
    }

    free(tmp);
}

int find_scgfx_metadata_in_track(const uint8_t *track, size_t track_len,
                                        ScgfxMetadata *meta) {
    size_t p = 0;
    uint8_t running_status = 0;

    while (p < track_len) {
        uint32_t delta = 0;

        if (!read_varlen_from_buf(track, track_len, &p, &delta))
            return 0;

        if (p >= track_len)
            break;

        uint8_t first = track[p];
        uint8_t status;

        if (first >= 0x80) {
            status = first;
            p++;
        } else {
            if (!running_status)
                return 0;
            status = running_status;
        }

        if (status < 0xF0) {
            int n = midi_channel_data_len(status);
            if (n < 0 || p + n > track_len)
                return 0;

            p += n;
            running_status = status;
        } else if (status == 0xFF) {
            if (p >= track_len)
                return 0;

            uint8_t type = track[p++];
            uint32_t len = 0;

            if (!read_varlen_from_buf(track, track_len, &p, &len) || p + len > track_len)
                return 0;

            if (type == 0x01 || type == 0x03)
                parse_scgfx_metadata_text((const char *)(track + p), len, meta);

            p += len;
            running_status = 0;

            if (type == 0x2F)
                return 1;
        } else if (status == 0xF0 || status == 0xF7) {
            uint32_t len = 0;

            if (!read_varlen_from_buf(track, track_len, &p, &len) || p + len > track_len)
                return 0;

            p += len;
            running_status = 0;
        } else {
            return 0;
        }
    }

    return 1;
}

int find_scgfx_metadata_in_midi(const uint8_t *data, size_t size,
                                       ScgfxMetadata *meta) {
    memset(meta, 0, sizeof(*meta));

    if (size < 14 || memcmp(data, "MThd", 4) != 0)
        return 0;

    uint32_t header_len = read_be32(data + 4);
    if (header_len < 6 || size < 8 + header_len)
        return 0;

    uint16_t tracks = read_be16(data + 10);
    size_t pos = 8 + header_len;

    for (uint16_t t = 0; t < tracks; t++) {
        if (pos + 8 > size || memcmp(data + pos, "MTrk", 4) != 0)
            return 0;

        uint32_t track_len = read_be32(data + pos + 4);

        if (pos + 8 + track_len > size)
            return 0;

        find_scgfx_metadata_in_track(data + pos + 8, track_len, meta);

        pos += 8 + track_len;
    }

    return meta->found;
}

