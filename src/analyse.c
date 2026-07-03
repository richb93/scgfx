#include "scgfx_internal.h"

void describe_dt1_address(uint8_t a0, uint8_t a1, uint8_t a2,
                                 const uint8_t *payload,
                                 uint32_t payload_len,
                                 char *desc,
                                 size_t desc_len) {
    if (a0 == 0x10 && a1 == 0x00 && a2 == 0x00) {
        char text[33];
        size_t n = payload_len;

        if (n > 32)
            n = 32;

        for (size_t i = 0; i < n; i++) {
            unsigned char c = payload[i];
            text[i] = (c >= 32 && c <= 126) ? (char)c : '.';
        }

        text[n] = 0;

        snprintf(desc, desc_len, "Instrument text \"%s\"", text);
    } else if (a0 == 0x10 && a1 == 0x20 && a2 == 0x00 && payload_len >= 1) {
        uint8_t page = payload[0];

        if (page == 0)
            snprintf(desc, desc_len, "Display Page 0 / bar display");
        else if (page <= 10)
            snprintf(desc, desc_len, "Display Page %u", page);
        else
            snprintf(desc, desc_len, "Display Page value %u", page);
    } else if (a0 == 0x10 && a1 == 0x20 && a2 == 0x01 && payload_len >= 1) {
        snprintf(desc, desc_len, "Display Time %u", payload[0]);
    } else if (a0 == 0x10 && a1 >= 0x01 && a1 <= 0x05 &&
               (a2 == 0x00 || a2 == 0x40)) {
        int page = ((int)a1 * 2) - (a2 == 0x00 ? 1 : 0);

        if (payload_len == 64)
            snprintf(desc, desc_len, "Page %d dot data, full frame", page);
        else
            snprintf(desc, desc_len, "Page %d dot data, %u byte partial write", page, payload_len);
    } else {
        snprintf(desc, desc_len, "DT1 data");
    }
}

int dump_display_dt1_from_track(const uint8_t *track, size_t track_len,
                                       uint16_t track_no,
                                       int *shown) {
    size_t p = 0;
    uint32_t tick = 0;
    uint8_t running_status = 0;

    while (p < track_len) {
        uint32_t delta = 0;

        if (!read_varlen_from_buf(track, track_len, &p, &delta))
            return 0;

        tick += delta;

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

            if ((type == 0x01 || type == 0x03) && len >= 6 &&
                memcmp(track + p, "SCGFX:", 6) == 0) {
                dump_printf("%06u T%u META %.*s\n", tick, track_no, (int)len, track + p);
                (*shown)++;
            }

            p += len;
            running_status = 0;

            if (type == 0x2F)
                return 1;
        } else if (status == 0xF0 || status == 0xF7) {
            uint32_t len = 0;

            if (!read_varlen_from_buf(track, track_len, &p, &len) || p + len > track_len)
                return 0;

            if (status == 0xF0 && len >= 9 &&
                track[p + 0] == 0x41 &&
                track[p + 1] == 0x10 &&
                track[p + 2] == 0x45 &&
                track[p + 3] == 0x12) {
                uint8_t a0 = track[p + 4];
                uint8_t a1 = track[p + 5];
                uint8_t a2 = track[p + 6];
                uint32_t payload_len = len - 9;
                const uint8_t *payload = track + p + 7;
                uint8_t got_checksum = track[p + 7 + payload_len];
                uint8_t end = track[p + 8 + payload_len];
                uint8_t addr[3] = { a0, a1, a2 };
                uint8_t expected = roland_checksum(addr, payload, (int)payload_len);
                char desc[160];

                describe_dt1_address(a0, a1, a2, payload, payload_len,
                                     desc, sizeof(desc));

                dump_printf("%06u T%u DT1  %02X %02X %02X  len=%u  %s%s%s\n",
                       tick, track_no, a0, a1, a2, payload_len, desc,
                       end == 0xF7 ? "" : "  [BAD END]",
                       got_checksum == expected ? "" : "  [BAD CHECKSUM]");
                (*shown)++;
            }

            p += len;
            running_status = 0;
        } else {
            return 0;
        }
    }

    return 1;
}

int dump_display_dt1_from_midi(const uint8_t *data, size_t size) {
    if (size < 14 || memcmp(data, "MThd", 4) != 0) {
        fprintf(stderr, "Input is not a MIDI file\n");
        return 0;
    }

    uint32_t header_len = read_be32(data + 4);
    if (header_len < 6 || size < 8 + header_len) {
        fprintf(stderr, "Bad MIDI header\n");
        return 0;
    }

    uint16_t tracks = read_be16(data + 10);
    size_t pos = 8 + header_len;
    int shown = 0;

    dump_printf("\nDisplay/SysEx dump:\n");

    for (uint16_t t = 0; t < tracks; t++) {
        if (pos + 8 > size || memcmp(data + pos, "MTrk", 4) != 0) {
            fprintf(stderr, "Could not find MTrk %u\n", t);
            return 0;
        }

        uint32_t track_len = read_be32(data + pos + 4);

        if (pos + 8 + track_len > size) {
            fprintf(stderr, "Track %u length exceeds file\n", t);
            return 0;
        }

        if (!dump_display_dt1_from_track(data + pos + 8, track_len, t + 1, &shown)) {
            fprintf(stderr, "Could not parse track %u for dump\n", t + 1);
            return 0;
        }

        pos += 8 + track_len;
    }

    if (!shown)
        dump_printf("  No SCGFX/Roland display DT1 events found.\n");

    return 1;
}

int analyse_command(int argc, char **argv) {
    const char *input_mid = NULL;
    const char *log_path = NULL;
    int dump = 0;
    int arg = 2;

    while (arg < argc) {
        if (strcmp(argv[arg], "-dump") == 0 || strcmp(argv[arg], "-verbose") == 0 || strcmp(argv[arg], "-v") == 0) {
            dump = 1;
            arg++;
        } else if (strcmp(argv[arg], "-log") == 0) {
            if (arg + 1 >= argc) {
                fprintf(stderr, "Usage:\n  scgfx analyse [-dump] [-log file] input.mid\n");
                return 1;
            }
            log_path = argv[arg + 1];
            arg += 2;
        } else {
            input_mid = argv[arg++];
            break;
        }
    }

    if (!input_mid || arg != argc) {
        fprintf(stderr, "Usage:\n  scgfx analyse [-dump] [-log file] input.mid\n");
        return 1;
    }

    uint8_t *data = NULL;
    size_t size = 0;
    if (!read_file(input_mid, &data, &size))
        return 1;

    ScgfxMetadata metadata;
    find_scgfx_metadata_in_midi(data, size, &metadata);

    GfxWriteList writes;
    memset(&writes, 0, sizeof(writes));

    int checksum_errors = 0;
    int range_errors = 0;

    if (!collect_graphics_writes_from_midi(data, size, &writes,
                                           &checksum_errors, &range_errors)) {
        gfxwrite_free(&writes);
        free(data);
        return 1;
    }

    if (writes.count == 0) {
        printf("Graphics DT1 writes found:  0\n");
        gfxwrite_free(&writes);
        free(data);
        return 0;
    }

    qsort(writes.items, writes.count, sizeof(GfxWrite), compare_gfxwrites);

    uint32_t *ticks = NULL;
    size_t tick_count = 0;

    if (!unique_graphics_ticks(&writes, &ticks, &tick_count)) {
        gfxwrite_free(&writes);
        free(data);
        return 1;
    }

    int full_writes = 0;
    int partial_writes = 0;
    uint32_t min_tick = writes.items[0].tick;
    uint32_t max_tick = writes.items[writes.count - 1].tick;

    for (size_t i = 0; i < writes.count; i++) {
        if (writes.items[i].full)
            full_writes++;
        else
            partial_writes++;
    }

    int interval_occurrences = 0;
    uint32_t suggested_grid = most_common_interval(ticks, tick_count,
                                                   &interval_occurrences);

    uint32_t min_interval = 0, max_interval = 0, gcd_interval = 0;
    int unique_intervals = 0;

    interval_stats(ticks, tick_count,
                   &min_interval, &max_interval,
                   &unique_intervals, &gcd_interval);

    printf("MIDI graphics analysis:\n");

    if (metadata.found) {
        printf("  SCGFX metadata:            frames=%u fps=%u grid=%u delta=%d\n",
               metadata.frames, metadata.fps, metadata.grid, metadata.delta);
    }

    printf("  Graphics DT1 writes:       %zu\n", writes.count);
    printf("  Graphics update ticks:     %zu\n", tick_count);
    printf("  Full framebuffer writes:   %d\n", full_writes);
    printf("  Partial/delta writes:      %d\n", partial_writes);
    printf("  First graphics tick:       %u\n", min_tick);
    printf("  Last graphics tick:        %u\n", max_tick);

    if (tick_count >= 2) {
        printf("  Min update interval:       %u ticks\n", min_interval);
        printf("  Max update interval:       %u ticks\n", max_interval);
        printf("  Unique update intervals:   %d\n", unique_intervals);
        printf("  GCD update interval:       %u ticks\n", gcd_interval);
    }

    if (suggested_grid) {
        double confidence = 0.0;

        if (tick_count > 1)
            confidence = (100.0 * interval_occurrences) / (double)(tick_count - 1);

        printf("  Most common interval:      %u ticks (%d occurrence%s, %.1f%%)\n",
               suggested_grid,
               interval_occurrences,
               interval_occurrences == 1 ? "" : "s",
               confidence);

        if (metadata.found && metadata.grid > 0 && metadata.frames > 0) {
            printf("\nRecommendation:\n");
            printf("  This file contains scgfx metadata, so decode can reconstruct the original frame count.\n");
            printf("  Use:\n");
            printf("    scgfx decode %s\n", input_mid);
        } else if (partial_writes == 0 && full_writes > 0) {
            printf("\nRecommendation:\n");
            printf("  This file appears to contain explicit full graphics frames.\n");
            printf("  Decode without -grid to preserve the graphics events exactly:\n");
            printf("    scgfx decode %s\n", input_mid);
        } else if (confidence < 80.0 || unique_intervals > 3) {
            printf("\nRecommendation:\n");
            printf("  Graphics timing appears irregular/jittery.\n");
            printf("  Decode without -grid for one frame per graphics update, or choose a grid manually.\n");
            printf("  Candidate grid: %u ticks\n", suggested_grid);
        } else {
            printf("  Suggested decode grid:     %u ticks\n", suggested_grid);
            printf("\nTry:\n");
            printf("  scgfx decode -grid %u %s\n", suggested_grid, input_mid);
        }
    } else {
        printf("  Suggested decode grid:     unavailable\n");
    }

    if (checksum_errors)
        printf("  Skipped checksum errors:   %d\n", checksum_errors);

    if (range_errors)
        printf("  Skipped address/range errs:%d\n", range_errors);

    if (dump) {
        if (log_path) {
            g_dump_log = fopen(log_path, "w");
            if (!g_dump_log) {
                perror(log_path);
            } else {
                dump_display_dt1_from_midi(data, size);
                fclose(g_dump_log);
                g_dump_log = NULL;
                printf("Display/SysEx dump written to: %s\n", log_path);
            }
        } else {
            dump_display_dt1_from_midi(data, size);
        }
    }

    free(ticks);
    gfxwrite_free(&writes);
    free(data);

    return 0;
}

