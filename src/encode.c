#include "scgfx_internal.h"

int write_interleaved_format0(FILE *out, const uint8_t *midi_data, size_t midi_size, int fps, int delta_mode, int frame_arg, int argc, char **argv) {
    if (midi_size < 14 || memcmp(midi_data, "MThd", 4) != 0) { fprintf(stderr, "Input is not a MIDI file\n"); return 0; }
    uint32_t header_len = read_be32(midi_data + 4);
    if (header_len < 6 || midi_size < 8 + header_len) { fprintf(stderr, "Bad MIDI header\n"); return 0; }
    uint16_t format = read_be16(midi_data + 8), tracks = read_be16(midi_data + 10), division = read_be16(midi_data + 12);
    if (format != 0 || tracks != 1) { fprintf(stderr, "-interleave currently supports Format 0 MIDI files only.\nUse normal merge mode, or convert the MIDI to Format 0 first.\n"); return 0; }
    size_t pos = 8 + header_len;
    if (pos + 8 > midi_size || memcmp(midi_data + pos, "MTrk", 4) != 0) { fprintf(stderr, "Could not find MTrk chunk\n"); return 0; }
    uint32_t track_len = read_be32(midi_data + pos + 4);
    if (pos + 8 + track_len > midi_size) { fprintf(stderr, "Track length exceeds file\n"); return 0; }

    EventList events; memset(&events, 0, sizeof(events));
    uint32_t last_music_tick = 0;
    if (!parse_format0_track_events(midi_data + pos + 8, track_len, &events, &last_music_tick)) { eventlist_free(&events); return 0; }

    uint32_t ticks_per_frame = (division * 2) / fps;
    if (ticks_per_frame < 1) ticks_per_frame = 1;

    uint32_t order = 1000000000u;

    {
        char meta_text[256];
        uint32_t evlen = 0;
        int frame_count = argc - frame_arg;

        make_scgfx_metadata_text(meta_text, sizeof(meta_text),
                                 frame_count, fps, ticks_per_frame, delta_mode);

        uint8_t *ev = make_meta_text_event_data(meta_text, &evlen);
        if (!ev) {
            eventlist_free(&events);
            return 0;
        }

        if (!eventlist_add_take(&events, 0, ev, evlen, order++)) {
            free(ev);
            eventlist_free(&events);
            return 0;
        }
    }

    uint8_t prev_fb[64]; memset(prev_fb, 0, 64);
    int have_prev = 0, total_packets = 0;
    DeltaStats stats; memset(&stats, 0, sizeof(stats));

    for (int i = frame_arg; i < argc; i++) {
        uint8_t pixels[16][16], fb[64];
        uint32_t tick = (uint32_t)(i - frame_arg) * ticks_per_frame;
        if (!load_frame(argv[i], pixels)) { eventlist_free(&events); return 0; }
        build_framebuffer(pixels, fb);
        if (delta_mode) { stats.frames_total++; stats.full_equivalent_bytes += FULL_FRAME_EVENT_BYTES; }
        if (!delta_mode || !have_prev) {
            uint32_t evlen = 0;
            uint8_t *ev = make_sc55_event_data(fb, &evlen);
            if (!ev) { eventlist_free(&events); return 0; }
            if (!eventlist_add_take(&events, tick, ev, evlen, order++)) { free(ev); eventlist_free(&events); return 0; }
            total_packets++;
            if (delta_mode) { stats.full_frames++; stats.full_packets++; stats.actual_graphics_bytes += evlen; }
        } else {
            int runs = add_delta_events(&events, tick, prev_fb, fb, &order, delta_mode ? &stats : NULL);
            if (runs < 0) { eventlist_free(&events); return 0; }
            total_packets += runs;
        }
        memcpy(prev_fb, fb, 64); have_prev = 1;
        if (g_verbose)
            printf("Interleaved frame %d: %s @ tick %u\n", i - frame_arg + 1, argv[i], tick);
        trace_printf("Interleaved frame %d source=%s tick=%u mode=%s\n",
                     i - frame_arg + 1, argv[i], tick, delta_mode ? "delta" : "full");
        if (!g_verbose)
            printf("Added interleaved graphics frame: %s\n", argv[i]);
    }

    qsort(events.events, events.count, sizeof(MidiEvent), compare_events);

    fwrite("MThd", 1, 4, out); write_be32(out, 6); write_be16(out, 0); write_be16(out, 1); write_be16(out, division);
    fwrite("MTrk", 1, 4, out);
    long size_pos = ftell(out); write_be32(out, 0); long track_start = ftell(out);
    uint32_t out_tick = 0;
    for (size_t i = 0; i < events.count; i++) {
        MidiEvent *ev = &events.events[i];
        write_varlen(out, ev->tick - out_tick);
        fwrite(ev->data, 1, ev->len, out);
        out_tick = ev->tick;
    }
    write_varlen(out, 0); fputc(0xFF, out); fputc(0x2F, out); fputc(0x00, out);
    long track_end = ftell(out); fseek(out, size_pos, SEEK_SET); write_be32(out, (uint32_t)(track_end - track_start));
    eventlist_free(&events);
    printf("Interleave mode: Format 0\n");
    printf("FPS: %d\n", fps);
    printf("Ticks per frame: %u\n", ticks_per_frame);
    if (delta_mode) {
        printf("Delta mode: on, %d DT1 packet(s) emitted. SC-88 or later only.\n", total_packets);
        print_delta_stats(&stats);
    }
    return 1;
}

int encode_command(int argc, char **argv) {
    int fps = DEFAULT_FPS, interleave = 0, delta_mode = 0, verbose = 0;
    const char *input_mid = NULL, *output_mid = NULL;
    int arg = 2;
    while (arg < argc) {
        if (strcmp(argv[arg], "-v") == 0 || strcmp(argv[arg], "-verbose") == 0) {
            verbose = 1; arg++;
        } else if (strcmp(argv[arg], "-vv") == 0) {
            verbose = 2; arg++;
        } else if (strcmp(argv[arg], "-fps") == 0) {
            if (arg + 1 >= argc) { fprintf(stderr, "Missing value after -fps\n"); return 1; }
            fps = atoi(argv[arg + 1]); if (fps <= 0) { fprintf(stderr, "FPS must be greater than zero\n"); return 1; } arg += 2;
        } else if (strcmp(argv[arg], "-interleave") == 0) { interleave = 1; arg++;
        } else if (strcmp(argv[arg], "-delta") == 0) { delta_mode = 1; arg++;
        } else if (strcmp(argv[arg], "-m") == 0) {
            if (arg + 1 >= argc) { fprintf(stderr, "Missing MIDI filename after -m\n"); return 1; }
            input_mid = argv[arg + 1]; arg += 2;
        } else if (strcmp(argv[arg], "-o") == 0) {
            if (arg + 1 >= argc) { fprintf(stderr, "Missing output filename after -o\n"); return 1; }
            output_mid = argv[arg + 1]; arg += 2;
        } else break;
    }
    if (!output_mid || arg >= argc) { usage(); return 1; }
    if (interleave && !input_mid) { fprintf(stderr, "-interleave requires -m input.mid\n"); return 1; }
    if (delta_mode) fprintf(stderr, "Warning: -delta is intended for SC-88 or later. It is not SC-55 compatible.\n");

    FILE *out = fopen(output_mid, "wb");
    if (!out) { perror(output_mid); return 1; }

    g_verbose = verbose;
    if (verbose >= 2)
        g_trace_log = open_auto_log(output_mid);

    if (verbose) {
        printf("Encode mode\n");
        printf("  Output:      %s\n", output_mid);
        printf("  Input MIDI:  %s\n", input_mid ? input_mid : "(none)");
        printf("  FPS:         %d\n", fps);
        printf("  Delta:       %s\n", delta_mode ? "on" : "off");
        printf("  Interleave:  %s\n", interleave ? "on" : "off");
    }

    trace_printf("scgfx encode trace\nOutput: %s\nInput MIDI: %s\nFPS: %d\nDelta: %s\nInterleave: %s\n\n",
                 output_mid, input_mid ? input_mid : "(none)", fps,
                 delta_mode ? "on" : "off", interleave ? "on" : "off");

    if (interleave) {
        uint8_t *midi_data = NULL; size_t midi_size = 0;
        if (!read_file(input_mid, &midi_data, &midi_size)) { fclose(out); return 1; }
        int ok = write_interleaved_format0(out, midi_data, midi_size, fps, delta_mode, arg, argc, argv);
        free(midi_data); fclose(out);
        if (!ok) {
            if (g_trace_log) { fclose(g_trace_log); g_trace_log = NULL; }
            return 1;
        }
        if (g_trace_log) { fclose(g_trace_log); g_trace_log = NULL; }
        printf("Generated %s\n", output_mid); return 0;
    }

    uint16_t division = TPQN;
    if (input_mid) {
        uint8_t *midi_data = NULL; size_t midi_size = 0;
        if (!read_file(input_mid, &midi_data, &midi_size)) { fclose(out); return 1; }
        if (!copy_existing_midi_as_type1(out, midi_data, midi_size, &division)) { free(midi_data); fclose(out); return 1; }
        free(midi_data);
    } else write_graphics_only_midi(out);

    uint32_t ticks_per_frame = (division * 2) / fps;
    if (ticks_per_frame < 1) ticks_per_frame = 1;

    fwrite("MTrk", 1, 4, out);
    long size_pos = ftell(out); write_be32(out, 0); long track_start = ftell(out);

    {
        char meta_text[256];
        int frame_count = argc - arg;

        make_scgfx_metadata_text(meta_text, sizeof(meta_text),
                                 frame_count, fps, ticks_per_frame, delta_mode);

        emit_meta_text_event(out, 0, meta_text);
    }

    uint8_t prev_fb[64]; memset(prev_fb, 0, 64);
    int have_prev = 0, last_sent_index = arg - 1, total_packets = 0;
    DeltaStats stats; memset(&stats, 0, sizeof(stats));

    for (int i = arg; i < argc; i++) {
        uint8_t pixels[16][16], fb[64];
        if (!load_frame(argv[i], pixels)) { fclose(out); return 1; }
        build_framebuffer(pixels, fb);
        if (delta_mode) { stats.frames_total++; stats.full_equivalent_bytes += FULL_FRAME_EVENT_BYTES; }
        if (!delta_mode || !have_prev) {
            uint32_t delay = (i == arg) ? 0 : (uint32_t)(i - last_sent_index) * ticks_per_frame;
            emit_sc55_frame(out, fb, delay);
            last_sent_index = i; total_packets++;
            if (delta_mode) { stats.full_frames++; stats.full_packets++; stats.actual_graphics_bytes += FULL_FRAME_EVENT_BYTES; }
        } else {
            uint32_t delay = (uint32_t)(i - last_sent_index) * ticks_per_frame;
            int runs = emit_delta_frame_file(out, prev_fb, fb, delay, delta_mode ? &stats : NULL);
            if (runs > 0) { last_sent_index = i; total_packets += runs; }
        }
        memcpy(prev_fb, fb, 64); have_prev = 1;
        if (verbose)
            printf("Frame %d: %s%s\n", i - arg + 1, argv[i], delta_mode ? " (delta)" : "");
        trace_printf("Frame %d source=%s tick=%u mode=%s\n",
                     i - arg + 1, argv[i],
                     (unsigned)((i - arg) * ticks_per_frame),
                     delta_mode ? "delta" : "full");
        if (!verbose)
            printf("Added frame: %s\n", argv[i]);
    }

    write_varlen(out, 0); fputc(0xFF, out); fputc(0x2F, out); fputc(0x00, out);
    long track_end = ftell(out); fseek(out, size_pos, SEEK_SET); write_be32(out, (uint32_t)(track_end - track_start));
    fclose(out);
    printf("Generated %s\n", output_mid);
    printf("FPS: %d\n", fps);
    printf("Ticks per frame: %u\n", ticks_per_frame);
    if (delta_mode) {
        printf("Delta mode: on, %d DT1 packet(s) emitted. SC-88 or later only.\n", total_packets);
        print_delta_stats(&stats);
    }
    trace_printf("\nSummary:\nFPS=%d\nTicks per frame=%u\nDT1 packets=%d\n", fps, ticks_per_frame, total_packets);
    if (g_trace_log) { fclose(g_trace_log); g_trace_log = NULL; }
    return 0;
}

