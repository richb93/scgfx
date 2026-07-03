#include "scgfx_internal.h"

int build_delta_runs(const uint8_t prev[64], const uint8_t curr[64],
                            DeltaRun runs[64], DeltaStats *stats)
{
    int raw_count = 0;
    int merged_count = 0;

    for (int i = 0; i < 64; ) {
        if (prev[i] == curr[i]) {
            i++;
            continue;
        }

        int start = i;
        while (i < 64 && prev[i] != curr[i])
            i++;

        int len = i - start;
        raw_count++;

        if (merged_count > 0) {
            DeltaRun *last = &runs[merged_count - 1];
            int last_end = last->start + last->len;
            int gap = start - last_end;

            /*
                Gap merging:

                A DT1 packet has about 11 bytes of event overhead:
                    F0, length, Roland/model/command/address, checksum, F7

                If two changed runs are separated by a small unchanged gap,
                it can be smaller to transmit one larger run including the
                unchanged bytes than to send another whole DT1 packet.
            */
            if (gap <= DELTA_MERGE_GAP) {
                last->len = (start + len) - last->start;
                last->changed_bytes += len;
                continue;
            }
        }

        runs[merged_count].start = start;
        runs[merged_count].len = len;
        runs[merged_count].changed_bytes = len;
        merged_count++;
    }

    if (stats)
        stats->raw_changed_runs += (uint32_t)raw_count;

    return merged_count;
}

uint32_t delta_runs_event_cost(const DeltaRun *runs, int count)
{
    uint32_t cost = 0;

    for (int i = 0; i < count; i++)
        cost += DT1_EVENT_OVERHEAD_BYTES + (uint32_t)runs[i].len;

    return cost;
}

int emit_delta_frame_file(FILE *out, const uint8_t prev[64], const uint8_t curr[64],
                                 uint32_t delay, DeltaStats *stats)
{
    DeltaRun runs[64];
    int count = build_delta_runs(prev, curr, runs, stats);

    if (count == 0) {
        if (stats)
            stats->identical_frames++;
        return 0;
    }

    uint32_t delta_cost = delta_runs_event_cost(runs, count);

    if (delta_cost >= FULL_FRAME_EVENT_BYTES) {
        emit_sc55_frame(out, curr, delay);
        if (stats) {
            stats->full_frames++;
            stats->full_packets++;
            stats->full_frame_fallbacks++;
            stats->actual_graphics_bytes += FULL_FRAME_EVENT_BYTES;
        }
        return 1;
    }

    for (int i = 0; i < count; i++) {
        emit_dt1(out, (uint8_t)runs[i].start, curr + runs[i].start,
                 runs[i].len, i == 0 ? delay : 0);

        if (stats) {
            stats->delta_packets++;
            stats->merged_changed_runs++;
            stats->changed_bytes += (uint32_t)runs[i].changed_bytes;
            stats->transmitted_delta_bytes += (uint32_t)runs[i].len;
            stats->actual_graphics_bytes += DT1_EVENT_OVERHEAD_BYTES + (uint32_t)runs[i].len;
        }
    }

    if (stats)
        stats->delta_frames++;

    return count;
}

int add_delta_events(EventList *events, uint32_t tick, const uint8_t prev[64],
                            const uint8_t curr[64], uint32_t *order, DeltaStats *stats)
{
    DeltaRun runs[64];
    int count = build_delta_runs(prev, curr, runs, stats);

    if (count == 0) {
        if (stats)
            stats->identical_frames++;
        return 0;
    }

    uint32_t delta_cost = delta_runs_event_cost(runs, count);

    if (delta_cost >= FULL_FRAME_EVENT_BYTES) {
        uint32_t evlen = 0;
        uint8_t *ev = make_sc55_event_data(curr, &evlen);
        if (!ev)
            return -1;

        if (!eventlist_add_take(events, tick, ev, evlen, (*order)++)) {
            free(ev);
            return -1;
        }

        if (stats) {
            stats->full_frames++;
            stats->full_packets++;
            stats->full_frame_fallbacks++;
            stats->actual_graphics_bytes += evlen;
        }

        return 1;
    }

    for (int i = 0; i < count; i++) {
        uint32_t evlen = 0;
        uint8_t *ev = make_dt1_event_data((uint8_t)runs[i].start,
                                          curr + runs[i].start,
                                          runs[i].len,
                                          &evlen);
        if (!ev)
            return -1;

        if (!eventlist_add_take(events, tick, ev, evlen, (*order)++)) {
            free(ev);
            return -1;
        }

        if (stats) {
            stats->delta_packets++;
            stats->merged_changed_runs++;
            stats->changed_bytes += (uint32_t)runs[i].changed_bytes;
            stats->transmitted_delta_bytes += (uint32_t)runs[i].len;
            stats->actual_graphics_bytes += evlen;
        }
    }

    if (stats)
        stats->delta_frames++;

    return count;
}

void print_delta_stats(const DeltaStats *stats)
{
    if (!stats || stats->frames_total == 0)
        return;

    printf("Delta statistics:\n");
    printf("  Encoding mode:            Delta (SC-88 or later)\n");
    printf("  Frames read:              %u\n", stats->frames_total);
    printf("  Full frames sent:         %u\n", stats->full_frames);
    printf("  Delta frames sent:        %u\n", stats->delta_frames);
    printf("  Identical frames skipped: %u\n", stats->identical_frames);
    printf("  Full-frame fallbacks:     %u\n", stats->full_frame_fallbacks);
    printf("  Full DT1 packets:         %u\n", stats->full_packets);
    printf("  Delta DT1 packets:        %u\n", stats->delta_packets);
    printf("  Raw changed runs:         %u\n", stats->raw_changed_runs);
    printf("  Merged changed runs:      %u\n", stats->merged_changed_runs);
    printf("  Changed framebuffer bytes:%u\n", stats->changed_bytes);
    printf("  Transmitted delta bytes:  %u\n", stats->transmitted_delta_bytes);

    if (stats->delta_frames > 0) {
        double avg_bytes = (double)stats->changed_bytes / (double)stats->delta_frames;
        double avg_runs = (double)stats->merged_changed_runs / (double)stats->delta_frames;
        printf("  Avg changed bytes/frame: %.2f\n", avg_bytes);
        printf("  Avg DT1 runs/frame:      %.2f\n", avg_runs);
    }

    printf("  Graphics bytes, full mode:%llu\n", (unsigned long long)stats->full_equivalent_bytes);
    printf("  Graphics bytes, actual:   %llu\n", (unsigned long long)stats->actual_graphics_bytes);

    if (stats->full_equivalent_bytes > 0 && stats->actual_graphics_bytes <= stats->full_equivalent_bytes) {
        uint64_t saved = stats->full_equivalent_bytes - stats->actual_graphics_bytes;
        double pct = 100.0 * (double)saved / (double)stats->full_equivalent_bytes;
        printf("  Graphics-event saving:    %llu bytes (%.1f%%)\n", (unsigned long long)saved, pct);

        if (stats->full_packets + stats->delta_packets > stats->frames_total) {
            uint32_t extra_packets = (stats->full_packets + stats->delta_packets) - stats->frames_total;
            printf("  Extra DT1 packet count:   %u\n", extra_packets);
            printf("  Note: total file saving may be lower than graphics-event saving\n");
            printf("        because extra DT1 packets also need MIDI delta-time bytes.\n");
        }
    }
}

