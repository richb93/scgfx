#include "scgfx_internal.h"

int script_event_add_take(ScriptEventList *list, uint32_t tick,
                                 uint8_t *data, uint32_t len,
                                 uint32_t order) {
    if (list->count == list->cap) {
        size_t new_cap = list->cap ? list->cap * 2 : 64;
        ScriptEvent *new_items = (ScriptEvent *)realloc(list->items,
                                                        new_cap * sizeof(ScriptEvent));
        if (!new_items)
            return 0;

        list->items = new_items;
        list->cap = new_cap;
    }

    list->items[list->count].tick = tick;
    list->items[list->count].order = order;
    list->items[list->count].data = data;
    list->items[list->count].len = len;
    list->count++;

    return 1;
}

void script_eventlist_free(ScriptEventList *list) {
    for (size_t i = 0; i < list->count; i++)
        free(list->items[i].data);

    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

int load_script_events(const char *filename,
                              ScriptEventList *script_events) {
    FILE *f = fopen(filename, "r");
    char line[1024];
    int line_no = 0;
    uint32_t order = 2000000000u;

    if (!f) {
        perror(filename);
        return 0;
    }

    while (fgets(line, sizeof(line), f)) {
        line_no++;

        char *p = trim_left(line);
        trim_right(p);

        if (*p == 0 || *p == '#')
            continue;

        char *endp = NULL;
        unsigned long tick_ul = strtoul(p, &endp, 10);

        if (endp == p) {
            fprintf(stderr, "%s:%d: expected tick number\n", filename, line_no);
            fclose(f);
            return 0;
        }

        uint32_t tick = (uint32_t)tick_ul;
        p = trim_left(endp);

        char cmd[64];
        int ci = 0;

        while (*p && !isspace((unsigned char)*p)) {
            if (ci < (int)sizeof(cmd) - 1)
                cmd[ci++] = *p;
            p++;
        }

        cmd[ci] = 0;
        p = trim_left(p);

        uint8_t *ev = NULL;
        uint32_t evlen = 0;

        if (strcmp(cmd, "text") == 0) {
            char msg[256];

            if (!parse_quoted_string(p, msg, sizeof(msg))) {
                fprintf(stderr, "%s:%d: expected quoted text, e.g. text \"HELLO\"\n",
                        filename, line_no);
                fclose(f);
                return 0;
            }

            size_t msg_len = strlen(msg);

            if (msg_len > 32) {
                fprintf(stderr, "%s:%d: text is %zu chars; truncating to 32\n",
                        filename, line_no, msg_len);
                msg_len = 32;
            }

            ev = make_dt1_event_data_addr(0x10, 0x00, 0x00,
                                     (const uint8_t *)msg,
                                     (uint32_t)msg_len,
                                     &evlen);
        } else if (strcmp(cmd, "page") == 0) {
            int page = atoi(p);

            if (page < 0 || page > 10) {
                fprintf(stderr, "%s:%d: page must be 0-10\n", filename, line_no);
                fclose(f);
                return 0;
            }

            uint8_t v = (uint8_t)page;
            ev = make_dt1_event_data_addr(0x10, 0x20, 0x00, &v, 1, &evlen);
        } else if (strcmp(cmd, "time") == 0) {
            int t = 0;

            if (!parse_display_time_value(p, &t)) {
                fprintf(stderr,
                        "%s:%d: time must be 0-15, or seconds/ms such as 2.88s or 1500ms\n",
                        filename, line_no);
                fclose(f);
                return 0;
            }

            uint8_t v = (uint8_t)t;
            ev = make_dt1_event_data_addr(0x10, 0x20, 0x01, &v, 1, &evlen);
        } else {
            fprintf(stderr, "%s:%d: unknown script command '%s'\n",
                    filename, line_no, cmd);
            fclose(f);
            return 0;
        }

        if (!ev) {
            fclose(f);
            return 0;
        }

        if (!script_event_add_take(script_events, tick, ev, evlen, order++)) {
            free(ev);
            fclose(f);
            return 0;
        }
    }

    fclose(f);
    return 1;
}

int script_command(int argc, char **argv) {
    const char *input_mid = NULL;
    const char *output_mid = NULL;
    const char *script_file = NULL;
    int arg = 2;

    while (arg < argc) {
        if (strcmp(argv[arg], "-m") == 0) {
            if (arg + 1 >= argc) {
                fprintf(stderr, "Missing MIDI filename after -m\n");
                return 1;
            }

            input_mid = argv[arg + 1];
            arg += 2;
        } else if (strcmp(argv[arg], "-o") == 0) {
            if (arg + 1 >= argc) {
                fprintf(stderr, "Missing output filename after -o\n");
                return 1;
            }

            output_mid = argv[arg + 1];
            arg += 2;
        } else {
            script_file = argv[arg++];
            break;
        }
    }

    if (!input_mid || !output_mid || !script_file) {
        fprintf(stderr,
            "Usage:\n"
            "  scgfx script -m input.mid -o output.mid events.txt\n\n"
            "Script examples:\n"
            "  0    text \"HELLO\"\n"
            "  480  text \"SC-55 GRAPHICS\"\n"
            "  1200 page 1\n"
            "  1200 time 2.88s\n");
        return 1;
    }

    uint8_t *midi_data = NULL;
    size_t midi_size = 0;

    if (!read_file(input_mid, &midi_data, &midi_size))
        return 1;

    if (midi_size < 14 || memcmp(midi_data, "MThd", 4) != 0) {
        fprintf(stderr, "Input is not a MIDI file\n");
        free(midi_data);
        return 1;
    }

    uint32_t header_len = read_be32(midi_data + 4);

    if (header_len < 6 || midi_size < 8 + header_len) {
        fprintf(stderr, "Bad MIDI header\n");
        free(midi_data);
        return 1;
    }

    uint16_t format = read_be16(midi_data + 8);
    uint16_t tracks = read_be16(midi_data + 10);
    uint16_t division = read_be16(midi_data + 12);

    if (format != 0 || tracks != 1) {
        fprintf(stderr,
                "script mode currently supports Format 0 MIDI files only.\n"
                "Convert to Format 0 first, or use encode merge mode for graphics.\n");
        free(midi_data);
        return 1;
    }

    size_t pos = 8 + header_len;

    if (pos + 8 > midi_size || memcmp(midi_data + pos, "MTrk", 4) != 0) {
        fprintf(stderr, "Could not find MTrk chunk\n");
        free(midi_data);
        return 1;
    }

    uint32_t track_len = read_be32(midi_data + pos + 4);

    if (pos + 8 + track_len > midi_size) {
        fprintf(stderr, "Track length exceeds file\n");
        free(midi_data);
        return 1;
    }

    EventList events;
    memset(&events, 0, sizeof(events));

    uint32_t last_music_tick = 0;

    if (!parse_format0_track_events(midi_data + pos + 8, track_len,
                                    &events, &last_music_tick)) {
        eventlist_free(&events);
        free(midi_data);
        return 1;
    }

    ScriptEventList script_events;
    memset(&script_events, 0, sizeof(script_events));

    if (!load_script_events(script_file, &script_events)) {
        script_eventlist_free(&script_events);
        eventlist_free(&events);
        free(midi_data);
        return 1;
    }

    size_t inserted = script_events.count;

    for (size_t i = 0; i < script_events.count; i++) {
        if (!eventlist_add_take(&events,
                                script_events.items[i].tick,
                                script_events.items[i].data,
                                script_events.items[i].len,
                                script_events.items[i].order)) {
            script_events.items[i].data = NULL;
            script_eventlist_free(&script_events);
            eventlist_free(&events);
            free(midi_data);
            return 1;
        }

        script_events.items[i].data = NULL;
    }

    script_eventlist_free(&script_events);

    qsort(events.events, events.count, sizeof(MidiEvent), compare_events);

    FILE *out = fopen(output_mid, "wb");
    if (!out) {
        perror(output_mid);
        eventlist_free(&events);
        free(midi_data);
        return 1;
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

    for (size_t i = 0; i < events.count; i++) {
        MidiEvent *ev = &events.events[i];

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

    eventlist_free(&events);
    free(midi_data);

    printf("Generated %s\n", output_mid);
    printf("Inserted %zu script event(s)\n", inserted);

    return 0;
}

