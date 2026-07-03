#include "scgfx_internal.h"

int gfxwrite_add(GfxWriteList *list, uint32_t tick, uint32_t order,
                        uint8_t offset, const uint8_t *data, uint8_t len, int full) {
    if (list->count == list->cap) {
        size_t new_cap = list->cap ? list->cap * 2 : 256;
        GfxWrite *new_items = (GfxWrite *)realloc(list->items, new_cap * sizeof(GfxWrite));
        if (!new_items)
            return 0;
        list->items = new_items;
        list->cap = new_cap;
    }

    list->items[list->count].tick = tick;
    list->items[list->count].order = order;
    list->items[list->count].offset = offset;
    list->items[list->count].len = len;
    list->items[list->count].full = full;
    memset(list->items[list->count].data, 0, 64);
    memcpy(list->items[list->count].data, data, len);
    list->count++;

    return 1;
}

void gfxwrite_free(GfxWriteList *list) {
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

int compare_gfxwrites(const void *a, const void *b) {
    const GfxWrite *wa = (const GfxWrite *)a;
    const GfxWrite *wb = (const GfxWrite *)b;

    if (wa->tick < wb->tick) return -1;
    if (wa->tick > wb->tick) return 1;
    if (wa->order < wb->order) return -1;
    if (wa->order > wb->order) return 1;
    return 0;
}

int try_add_graphics_sysex(const uint8_t *syx, uint32_t syx_len,
                                  uint32_t tick, uint32_t order,
                                  GfxWriteList *writes,
                                  int *checksum_errors,
                                  int *range_errors) {
    /*
        SMF SysEx payload after the F0 length should be:

            41 10 45 12 10 01 <offset> [data...] checksum F7

        Full frames have 64 data bytes. Delta frames have 1..64 data bytes.
    */

    if (syx_len < 10 || syx_len > 73)
        return 1;

    if (syx[0] != 0x41 ||
        syx[1] != 0x10 ||
        syx[2] != 0x45 ||
        syx[3] != 0x12)
        return 1;

    if (syx[4] != 0x10 || syx[5] != 0x01)
        return 1;

    uint8_t offset = syx[6];
    int dt1_len = (int)syx_len - 9;

    if (dt1_len <= 0)
        return 1;

    if (offset >= 64 || offset + dt1_len > 64) {
        (*range_errors)++;
        return 1;
    }

    const uint8_t *dt1_data = syx + 7;
    uint8_t checksum = syx[7 + dt1_len];
    uint8_t end = syx[8 + dt1_len];

    if (end != 0xF7)
        return 1;

    const uint8_t addr[3] = { 0x10, 0x01, offset };
    uint8_t expected = roland_checksum(addr, dt1_data, dt1_len);

    if (checksum != expected) {
        (*checksum_errors)++;
        return 1;
    }

    return gfxwrite_add(writes, tick, order, offset, dt1_data,
                        (uint8_t)dt1_len, offset == 0 && dt1_len == 64);
}

int collect_graphics_writes_from_track(const uint8_t *track, size_t track_len,
                                              GfxWriteList *writes,
                                              uint32_t *order_counter,
                                              int *checksum_errors,
                                              int *range_errors) {
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

            p += len;
            running_status = 0;

            if (type == 0x2F)
                return 1;
        } else if (status == 0xF0 || status == 0xF7) {
            uint32_t len = 0;

            if (!read_varlen_from_buf(track, track_len, &p, &len) || p + len > track_len)
                return 0;

            if (status == 0xF0) {
                if (!try_add_graphics_sysex(track + p, len, tick, (*order_counter)++,
                                            writes, checksum_errors, range_errors))
                    return 0;
            }

            p += len;
            running_status = 0;
        } else {
            return 0;
        }
    }

    return 1;
}

int collect_graphics_writes_from_midi(const uint8_t *data, size_t size,
                                             GfxWriteList *writes,
                                             int *checksum_errors,
                                             int *range_errors) {
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
    uint32_t order_counter = 0;

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

        if (!collect_graphics_writes_from_track(data + pos + 8, track_len,
                                                writes, &order_counter,
                                                checksum_errors, range_errors)) {
            fprintf(stderr, "Could not parse track %u\n", t);
            return 0;
        }

        pos += 8 + track_len;
    }

    return 1;
}

int unique_graphics_ticks(const GfxWriteList *writes,
                                 uint32_t **ticks_out,
                                 size_t *count_out) {
    if (writes->count == 0) {
        *ticks_out = NULL;
        *count_out = 0;
        return 1;
    }

    uint32_t *ticks = (uint32_t *)malloc(writes->count * sizeof(uint32_t));
    if (!ticks)
        return 0;

    size_t count = 0;

    for (size_t i = 0; i < writes->count; i++) {
        if (i == 0 || writes->items[i].tick != writes->items[i - 1].tick)
            ticks[count++] = writes->items[i].tick;
    }

    *ticks_out = ticks;
    *count_out = count;
    return 1;
}

uint32_t most_common_interval(const uint32_t *ticks, size_t count,
                                     int *occurrences_out) {
    uint32_t best_interval = 0;
    int best_count = 0;

    if (count < 2) {
        if (occurrences_out)
            *occurrences_out = 0;
        return 0;
    }

    for (size_t i = 1; i < count; i++) {
        uint32_t interval = ticks[i] - ticks[i - 1];

        if (interval == 0)
            continue;

        int n = 1;

        for (size_t j = i + 1; j < count; j++) {
            if (ticks[j] - ticks[j - 1] == interval)
                n++;
        }

        if (n > best_count) {
            best_count = n;
            best_interval = interval;
        }
    }

    if (occurrences_out)
        *occurrences_out = best_count;

    return best_interval;
}

void interval_stats(const uint32_t *ticks, size_t count,
                           uint32_t *min_out,
                           uint32_t *max_out,
                           int *unique_out,
                           uint32_t *gcd_out) {
    uint32_t minv = 0, maxv = 0, gcdv = 0;
    int unique = 0;

    if (count < 2) {
        *min_out = *max_out = *gcd_out = 0;
        *unique_out = 0;
        return;
    }

    for (size_t i = 1; i < count; i++) {
        uint32_t d = ticks[i] - ticks[i - 1];

        if (d == 0)
            continue;

        if (minv == 0 || d < minv)
            minv = d;

        if (d > maxv)
            maxv = d;

        int seen = 0;
        for (size_t j = 1; j < i; j++) {
            if (ticks[j] - ticks[j - 1] == d) {
                seen = 1;
                break;
            }
        }

        if (!seen)
            unique++;

        if (gcdv == 0) {
            gcdv = d;
        } else {
            uint32_t a = gcdv;
            uint32_t b = d;

            while (b) {
                uint32_t t = a % b;
                a = b;
                b = t;
            }

            gcdv = a;
        }
    }

    *min_out = minv;
    *max_out = maxv;
    *unique_out = unique;
    *gcd_out = gcdv;
}

int decode_command(int argc, char **argv) {
    const char *format = "pbm", *input_mid = NULL;
    uint32_t grid = 0;
    int arg = 2;

    while (arg < argc) {
        if (strcmp(argv[arg], "-format") == 0) {
            if (arg + 1 >= argc) {
                fprintf(stderr, "Missing value after -format\n");
                return 1;
            }
            format = argv[arg + 1];
            arg += 2;
        } else if (strcmp(argv[arg], "-grid") == 0) {
            if (arg + 1 >= argc) {
                fprintf(stderr, "Missing value after -grid\n");
                return 1;
            }
            grid = (uint32_t)atoi(argv[arg + 1]);
            if (grid == 0) {
                fprintf(stderr, "-grid value must be greater than zero\n");
                return 1;
            }
            arg += 2;
        } else {
            input_mid = argv[arg++];
            break;
        }
    }

    if (!input_mid) {
        usage();
        return 1;
    }

    int output_ascii = 0;
    if (strcmp(format, "pbm") == 0) output_ascii = 0;
    else if (strcmp(format, "ascii") == 0 || strcmp(format, "txt") == 0) output_ascii = 1;
    else {
        fprintf(stderr, "Unknown format: %s\n", format);
        return 1;
    }

    uint8_t *data = NULL;
    size_t size = 0;
    if (!read_file(input_mid, &data, &size))
        return 1;

    ScgfxMetadata metadata;
    find_scgfx_metadata_in_midi(data, size, &metadata);

    if (metadata.found && grid == 0 && metadata.grid > 0)
        grid = metadata.grid;

    char *outdir = make_output_dir(input_mid);
    if (!outdir) {
        free(data);
        return 1;
    }

    MKDIR(outdir);

    GfxWriteList writes;
    memset(&writes, 0, sizeof(writes));

    int checksum_errors = 0;
    int range_errors = 0;

    if (!collect_graphics_writes_from_midi(data, size, &writes,
                                           &checksum_errors, &range_errors)) {
        gfxwrite_free(&writes);
        free(outdir);
        free(data);
        return 1;
    }

    if (writes.count == 0) {
        printf("Decoded 0 frame(s) into folder: %s\n", outdir);
        if (checksum_errors)
            printf("Skipped checksum errors:    %d\n", checksum_errors);
        if (range_errors)
            printf("Skipped address/range errs: %d\n", range_errors);

        gfxwrite_free(&writes);
        free(outdir);
        free(data);
        return 1;
    }

    qsort(writes.items, writes.count, sizeof(GfxWrite), compare_gfxwrites);

    uint8_t current_fb[64];
    memset(current_fb, 0, sizeof(current_fb));

    int frame_no = 1;
    int full_writes = 0;
    int partial_writes = 0;
    int coalesced_writes = 0;
    int held_frames = 0;

    if (grid == 0) {
        /*
            Default delta-aware decode:

            Apply all graphics writes at the same absolute MIDI tick, then
            output one reconstructed full frame for that tick.
        */
        size_t i = 0;
        while (i < writes.count) {
            uint32_t tick = writes.items[i].tick;
            int writes_this_tick = 0;

            while (i < writes.count && writes.items[i].tick == tick) {
                GfxWrite *w = &writes.items[i];

                memcpy(current_fb + w->offset, w->data, w->len);

                if (w->full)
                    full_writes++;
                else
                    partial_writes++;

                writes_this_tick++;
                i++;
            }

            if (writes_this_tick > 1)
                coalesced_writes += writes_this_tick - 1;

            if (!output_frame_file(outdir, output_ascii, frame_no, current_fb)) {
                gfxwrite_free(&writes);
                free(outdir);
                free(data);
                return 1;
            }

            frame_no++;
        }
    } else {
        /*
            Grid decode.

            If the file was produced by scgfx v1.5 or later, the encoder stores
            metadata containing the original source frame count and grid. In
            that case we can reconstruct the exact original number of frames by
            sampling frame index N at:

                first_graphics_tick + (N * grid)

            This is the best round-trip mode for delta files, because identical
            source frames may have been intentionally omitted from the MIDI.

            If there is no scgfx metadata, fall back to event-aware grid decode:
            output every real graphics update tick and insert held frames when
            there are larger gaps.
        */
        if (metadata.found && metadata.frames > 0 && metadata.grid > 0) {
            size_t i = 0;
            uint32_t first_tick = writes.items[0].tick;

            for (uint32_t fno = 0; fno < metadata.frames; fno++) {
                uint32_t sample_tick = first_tick + (fno * grid);
                int writes_this_tick = 0;

                while (i < writes.count && writes.items[i].tick <= sample_tick) {
                    GfxWrite *w = &writes.items[i];

                    memcpy(current_fb + w->offset, w->data, w->len);

                    if (w->full)
                        full_writes++;
                    else
                        partial_writes++;

                    if (w->tick == sample_tick)
                        writes_this_tick++;

                    i++;
                }

                if (writes_this_tick > 1)
                    coalesced_writes += writes_this_tick - 1;

                if (fno > 0 && writes_this_tick == 0)
                    held_frames++;

                if (!output_frame_file(outdir, output_ascii, frame_no, current_fb)) {
                    gfxwrite_free(&writes);
                    free(outdir);
                    free(data);
                    return 1;
                }

                frame_no++;
            }
        } else {
            /*
                No exact scgfx frame-count metadata. Preserve all actual
                graphics update ticks and insert held frames when the gap
                between updates spans multiple grid intervals.
            */
            size_t i = 0;

            while (i < writes.count) {
                uint32_t tick = writes.items[i].tick;
                int writes_this_tick = 0;

                while (i < writes.count && writes.items[i].tick == tick) {
                    GfxWrite *w = &writes.items[i];

                    memcpy(current_fb + w->offset, w->data, w->len);

                    if (w->full)
                        full_writes++;
                    else
                        partial_writes++;

                    writes_this_tick++;
                    i++;
                }

                if (writes_this_tick > 1)
                    coalesced_writes += writes_this_tick - 1;

                if (!output_frame_file(outdir, output_ascii, frame_no, current_fb)) {
                    gfxwrite_free(&writes);
                    free(outdir);
                    free(data);
                    return 1;
                }

                frame_no++;

                if (i < writes.count) {
                    uint32_t next_tick = writes.items[i].tick;
                    uint32_t gap = next_tick - tick;
                    uint32_t intervals = (gap + (grid / 2)) / grid;

                    if (intervals > 1) {
                        for (uint32_t r = 1; r < intervals; r++) {
                            if (!output_frame_file(outdir, output_ascii, frame_no, current_fb)) {
                                gfxwrite_free(&writes);
                                free(outdir);
                                free(data);
                                return 1;
                            }

                            frame_no++;
                            held_frames++;
                        }
                    }
                }
            }
        }
    }

    printf("Decoded %d frame(s) into folder: %s\n", frame_no - 1, outdir);
    printf("Graphics DT1 writes found:  %zu\n", writes.count);
    printf("Full framebuffer writes:    %d\n", full_writes);
    printf("Partial/delta writes:       %d\n", partial_writes);
    printf("Coalesced same-tick writes: %d\n", coalesced_writes);

    if (metadata.found) {
        printf("SCGFX metadata:             frames=%u fps=%u grid=%u delta=%d\n",
               metadata.frames, metadata.fps, metadata.grid, metadata.delta);
    }

    if (grid) {
        printf("Decode grid:                %u ticks\n", grid);
        printf("Held/repeated frames:       %d\n", held_frames);
    }

    if (checksum_errors)
        printf("Skipped checksum errors:    %d\n", checksum_errors);

    if (range_errors)
        printf("Skipped address/range errs: %d\n", range_errors);

    if (!output_ascii) {
        printf("Make GIF with:\n");
        printf("  ffmpeg -framerate 10 -i %s/frame%%04d.pbm "
               "-vf \"scale=320:320:flags=neighbor\" %s.gif\n",
               outdir, outdir);
    }

    gfxwrite_free(&writes);
    free(outdir);
    free(data);

    return (frame_no > 1) ? 0 : 1;
}

