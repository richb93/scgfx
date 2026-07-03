#include "scgfx_internal.h"

int eventlist_add_take(EventList *list, uint32_t tick, uint8_t *data, uint32_t len, uint32_t order) {
    if (list->count == list->cap) {
        size_t nc = list->cap ? list->cap * 2 : 256;
        MidiEvent *ne = (MidiEvent *)realloc(list->events, nc * sizeof(MidiEvent));
        if (!ne) return 0;
        list->events = ne; list->cap = nc;
    }
    list->events[list->count].tick = tick;
    list->events[list->count].data = data;
    list->events[list->count].len = len;
    list->events[list->count].order = order;
    list->count++;
    return 1;
}

int eventlist_add_copy(EventList *list, uint32_t tick, const uint8_t *data, uint32_t len, uint32_t order) {
    uint8_t *copy = (uint8_t *)malloc(len);
    if (!copy) return 0;
    memcpy(copy, data, len);
    if (!eventlist_add_take(list, tick, copy, len, order)) { free(copy); return 0; }
    return 1;
}

void eventlist_free(EventList *list) {
    for (size_t i = 0; i < list->count; i++) free(list->events[i].data);
    free(list->events); list->events = NULL; list->count = 0; list->cap = 0;
}

int midi_channel_data_len(uint8_t status) {
    switch (status & 0xF0) {
        case 0xC0: case 0xD0: return 1;
        case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0: return 2;
    }
    return -1;
}

int parse_format0_track_events(const uint8_t *track, size_t track_len, EventList *events, uint32_t *last_tick_out) {
    size_t p = 0; uint32_t tick = 0; uint8_t running_status = 0; uint32_t order = 0;
    while (p < track_len) {
        uint32_t delta = 0;
        if (!read_varlen_from_buf(track, track_len, &p, &delta)) { fprintf(stderr, "Bad delta time\n"); return 0; }
        tick += delta;
        if (p >= track_len) break;
        uint8_t first = track[p], status;
        if (first >= 0x80) { status = first; p++; }
        else { if (!running_status) { fprintf(stderr, "Running status used before status byte\n"); return 0; } status = running_status; }

        if (status < 0xF0) {
            int n = midi_channel_data_len(status);
            if (n < 0 || p + n > track_len) { fprintf(stderr, "Bad MIDI channel event\n"); return 0; }
            uint8_t ev[3]; ev[0] = status;
            for (int i = 0; i < n; i++) ev[1 + i] = track[p++];
            running_status = status;
            if (!eventlist_add_copy(events, tick, ev, (uint32_t)(1 + n), order++)) return 0;
        } else if (status == 0xFF) {
            if (p >= track_len) { fprintf(stderr, "Bad meta event\n"); return 0; }
            uint8_t type = track[p++]; uint32_t len = 0;
            if (!read_varlen_from_buf(track, track_len, &p, &len) || p + len > track_len) { fprintf(stderr, "Bad meta event length\n"); return 0; }
            if (type == 0x2F) { *last_tick_out = tick; return 1; }
            uint8_t varbuf[5]; int varlen = encode_varlen_to_buf(len, varbuf);
            uint32_t evlen = 2 + (uint32_t)varlen + len;
            uint8_t *ev = (uint8_t *)malloc(evlen); if (!ev) return 0;
            int q = 0; ev[q++] = 0xFF; ev[q++] = type; memcpy(ev + q, varbuf, varlen); q += varlen; memcpy(ev + q, track + p, len);
            if (!eventlist_add_take(events, tick, ev, evlen, order++)) { free(ev); return 0; }
            p += len; running_status = 0;
        } else if (status == 0xF0 || status == 0xF7) {
            uint32_t len = 0;
            if (!read_varlen_from_buf(track, track_len, &p, &len) || p + len > track_len) { fprintf(stderr, "Bad SysEx event length\n"); return 0; }
            uint8_t varbuf[5]; int varlen = encode_varlen_to_buf(len, varbuf);
            uint32_t evlen = 1 + (uint32_t)varlen + len;
            uint8_t *ev = (uint8_t *)malloc(evlen); if (!ev) return 0;
            int q = 0; ev[q++] = status; memcpy(ev + q, varbuf, varlen); q += varlen; memcpy(ev + q, track + p, len);
            if (!eventlist_add_take(events, tick, ev, evlen, order++)) { free(ev); return 0; }
            p += len; running_status = 0;
        } else {
            fprintf(stderr, "Unsupported system event: 0x%02X\n", status); return 0;
        }
    }
    *last_tick_out = tick; return 1;
}

int compare_events(const void *a, const void *b) {
    const MidiEvent *ea = (const MidiEvent *)a, *eb = (const MidiEvent *)b;
    if (ea->tick < eb->tick) return -1;
    if (ea->tick > eb->tick) return 1;
    if (ea->order < eb->order) return -1;
    if (ea->order > eb->order) return 1;
    return 0;
}

int copy_existing_midi_as_type1(FILE *out, const uint8_t *data, size_t size, uint16_t *division_out) {
    if (size < 14 || memcmp(data, "MThd", 4) != 0) { fprintf(stderr, "Input is not a MIDI file\n"); return 0; }
    uint32_t header_len = read_be32(data + 4);
    if (header_len < 6 || size < 8 + header_len) { fprintf(stderr, "Bad MIDI header\n"); return 0; }
    uint16_t format = read_be16(data + 8), tracks = read_be16(data + 10), division = read_be16(data + 12);
    if (format > 1) { fprintf(stderr, "Format 2 MIDI not supported\n"); return 0; }
    *division_out = division;
    fwrite("MThd", 1, 4, out); write_be32(out, 6); write_be16(out, 1); write_be16(out, tracks + 1); write_be16(out, division);
    size_t pos = 8 + header_len;
    for (uint16_t t = 0; t < tracks; t++) {
        if (pos + 8 > size || memcmp(data + pos, "MTrk", 4) != 0) { fprintf(stderr, "Could not find MTrk %u\n", t); return 0; }
        uint32_t len = read_be32(data + pos + 4);
        if (pos + 8 + len > size) { fprintf(stderr, "Track %u length exceeds file\n", t); return 0; }
        fwrite(data + pos, 1, 8 + len, out); pos += 8 + len;
    }
    return 1;
}

int write_graphics_only_midi(FILE *out) {
    fwrite("MThd", 1, 4, out); write_be32(out, 6); write_be16(out, 0); write_be16(out, 1); write_be16(out, TPQN);
    return 1;
}

int write_format0_from_events(const char *output_mid,
                                     EventList *events,
                                     uint16_t division) {
    qsort(events->events, events->count, sizeof(MidiEvent), compare_events);

    FILE *out = fopen(output_mid, "wb");
    if (!out) {
        perror(output_mid);
        return 0;
    }

    fwrite("MThd", 1, 4, out);
    write_be32(out, 6);
    write_be16(out, 0);
    write_be16(out, 1);
    write_be16(out, division);

    fwrite("MTrk", 1, 4, out);

    long size_pos = ftell(out);
    write_be32(out, 0);
    long track_start = ftell(out);

    uint32_t out_tick = 0;

    for (size_t i = 0; i < events->count; i++) {
        MidiEvent *ev = &events->events[i];

        if (ev->tick < out_tick) {
            fprintf(stderr, "Internal event ordering error\n");
            fclose(out);
            return 0;
        }

        write_varlen(out, ev->tick - out_tick);
        fwrite(ev->data, 1, ev->len, out);
        out_tick = ev->tick;
    }

    write_varlen(out, 0);
    fputc(0xFF, out);
    fputc(0x2F, out);
    fputc(0x00, out);

    long track_end = ftell(out);

    fseek(out, size_pos, SEEK_SET);
    write_be32(out, (uint32_t)(track_end - track_start));

    fclose(out);
    return 1;
}

int load_format0_music_events(const char *input_mid,
                                     EventList *events,
                                     uint16_t *division_out) {
    uint8_t *midi_data = NULL;
    size_t midi_size = 0;

    if (!read_file(input_mid, &midi_data, &midi_size))
        return 0;

    if (midi_size < 14 || memcmp(midi_data, "MThd", 4) != 0) {
        fprintf(stderr, "Input is not a MIDI file\n");
        free(midi_data);
        return 0;
    }

    uint32_t header_len = read_be32(midi_data + 4);

    if (header_len < 6 || midi_size < 8 + header_len) {
        fprintf(stderr, "Bad MIDI header\n");
        free(midi_data);
        return 0;
    }

    uint16_t format = read_be16(midi_data + 8);
    uint16_t tracks = read_be16(midi_data + 10);
    uint16_t division = read_be16(midi_data + 12);

    if (format != 0 || tracks != 1) {
        fprintf(stderr,
                "Project/build mode currently supports Format 0 input MIDI only.\n"
                "Convert to Format 0 first.\n");
        free(midi_data);
        return 0;
    }

    size_t pos = 8 + header_len;

    if (pos + 8 > midi_size || memcmp(midi_data + pos, "MTrk", 4) != 0) {
        fprintf(stderr, "Could not find MTrk chunk\n");
        free(midi_data);
        return 0;
    }

    uint32_t track_len = read_be32(midi_data + pos + 4);

    if (pos + 8 + track_len > midi_size) {
        fprintf(stderr, "Track length exceeds file\n");
        free(midi_data);
        return 0;
    }

    uint32_t last_music_tick = 0;

    if (!parse_format0_track_events(midi_data + pos + 8, track_len,
                                    events, &last_music_tick)) {
        free(midi_data);
        return 0;
    }

    *division_out = division;
    free(midi_data);
    return 1;
}

