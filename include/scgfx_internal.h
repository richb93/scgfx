#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <stdarg.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(path) _mkdir(path)
#else
#define MKDIR(path) mkdir(path, 0755)
#endif

#define SCGFX_VERSION "3.1"
#define SCGFX_DATE    "3rd July 2026"
#define TPQN 96
#define DEFAULT_FPS 10
#define DELTA_MERGE_GAP 11
#define FULL_FRAME_EVENT_BYTES 75
#define DT1_EVENT_OVERHEAD_BYTES 11

/*
    scgfx - Roland Sound Canvas graphics encoder/decoder

    Roland SC-55 LCD Graphics SysEx:
        F0 41 10 45 12 10 01 00 [64 bytes] checksum F7

    Framebuffer layout, reverse engineered on real hardware:
        bytes  0-15  -> x  1-5,  y 1-16
        bytes 16-31  -> x  6-10, y 1-16
        bytes 32-47  -> x 11-15, y 1-16
        bytes 48-63  -> x 16-20, y 1-16

        bit4 = first column in group
        bit3 = second column
        bit2 = third column
        bit1 = fourth column
        bit0 = fifth column

    Visible display is 16x16, so x17-x20 are off-screen.

    Delta mode:
        The first frame is sent as a full 64-byte DT1 write.
        Later frames send only changed contiguous byte ranges.
        This is intended for SC-88 or later. It is not SC-55 compatible.

    Decode mode:
        Maintains a 64-byte framebuffer and applies every valid DT1 write
        into it before exporting a complete reconstructed 16x16 frame.
        This means it can decode both full-frame SC-55 files and delta /
        partial-write SC-88 files.
*/

typedef struct {
    uint32_t tick;
    uint8_t *data;
    uint32_t len;
    uint32_t order;
} MidiEvent;

typedef struct {
    MidiEvent *events;
    size_t count;
    size_t cap;
} EventList;

typedef struct {
    uint32_t frames_total;
    uint32_t full_frames;
    uint32_t delta_frames;
    uint32_t identical_frames;
    uint32_t full_packets;
    uint32_t delta_packets;
    uint32_t raw_changed_runs;
    uint32_t merged_changed_runs;
    uint32_t full_frame_fallbacks;
    uint32_t changed_bytes;
    uint32_t transmitted_delta_bytes;
    uint64_t full_equivalent_bytes;
    uint64_t actual_graphics_bytes;
} DeltaStats;

typedef struct {
    int start;
    int len;
    int changed_bytes;
} DeltaRun;


#ifndef SCGFX_INTERNAL_DECLS
#define SCGFX_INTERNAL_DECLS


typedef struct {
    int found;
    uint32_t frames;
    uint32_t fps;
    uint32_t grid;
    int delta;
} ScgfxMetadata;

typedef struct {
    uint32_t tick;
    uint32_t order;
    uint8_t offset;
    uint8_t len;
    uint8_t data[64];
    int full;
} GfxWrite;

typedef struct {
    GfxWrite *items;
    size_t count;
    size_t cap;
} GfxWriteList;

typedef struct {
    uint32_t tick;
    uint32_t order;
    uint8_t *data;
    uint32_t len;
} ScriptEvent;

typedef struct {
    ScriptEvent *items;
    size_t count;
    size_t cap;
} ScriptEventList;

extern FILE *g_trace_log;
extern int g_verbose;
extern FILE *g_dump_log;

void trace_printf(const char *fmt, ...);
FILE *open_auto_log(const char *output_name);
void dump_printf(const char *fmt, ...);
void usage(void);
void write_be16(FILE *f, uint16_t v);
void write_be32(FILE *f, uint32_t v);
uint16_t read_be16(const uint8_t *p);
uint32_t read_be32(const uint8_t *p);
void write_varlen(FILE *f, uint32_t v);
int encode_varlen_to_buf(uint32_t v, uint8_t *out);
int read_varlen_from_buf(const uint8_t *data, size_t size, size_t *pos, uint32_t *value);
uint8_t roland_checksum(const uint8_t addr[3], const uint8_t *data, int len);
int read_file(const char *name, uint8_t **data, size_t *size);
int pbm_next_token(FILE *f, char *buf, int buflen);
int load_txt(const char *filename, uint8_t pixels[16][16]);
int load_pbm_p1(const char *filename, uint8_t pixels[16][16]);
int load_pbm_p4(const char *filename, uint8_t pixels[16][16]);
int load_frame(const char *filename, uint8_t pixels[16][16]);
void build_framebuffer(uint8_t pixels[16][16], uint8_t fb[64]);
void framebuffer_to_pixels(const uint8_t fb[64], uint8_t pixels[16][16]);
void emit_dt1(FILE *f, uint8_t offset, const uint8_t *data, int len, uint32_t delay);
void emit_sc55_frame(FILE *f, const uint8_t fb[64], uint32_t delay);
uint8_t *make_dt1_event_data(uint8_t offset, const uint8_t *data, int len, uint32_t *out_len);
uint8_t *make_sc55_event_data(const uint8_t fb[64], uint32_t *out_len);
int build_delta_runs(const uint8_t prev[64], const uint8_t curr[64], DeltaRun runs[64], DeltaStats *stats);
uint32_t delta_runs_event_cost(const DeltaRun *runs, int count);
int emit_delta_frame_file(FILE *out, const uint8_t prev[64], const uint8_t curr[64], uint32_t delay, DeltaStats *stats);
int eventlist_add_take(EventList *list, uint32_t tick, uint8_t *data, uint32_t len, uint32_t order);
int eventlist_add_copy(EventList *list, uint32_t tick, const uint8_t *data, uint32_t len, uint32_t order);
int add_delta_events(EventList *events, uint32_t tick, const uint8_t prev[64], const uint8_t curr[64], uint32_t *order, DeltaStats *stats);
void eventlist_free(EventList *list);
int midi_channel_data_len(uint8_t status);
int parse_format0_track_events(const uint8_t *track, size_t track_len, EventList *events, uint32_t *last_tick_out);
int compare_events(const void *a, const void *b);
int copy_existing_midi_as_type1(FILE *out, const uint8_t *data, size_t size, uint16_t *division_out);
int write_graphics_only_midi(FILE *out);
void print_delta_stats(const DeltaStats *stats);
void make_scgfx_metadata_text(char *buf, size_t buflen, int frames, int fps, uint32_t grid, int delta_mode);
void emit_meta_text_event(FILE *f, uint32_t delay, const char *text);
uint8_t *make_meta_text_event_data(const char *text, uint32_t *out_len);
void parse_scgfx_metadata_text(const char *text, size_t len, ScgfxMetadata *meta);
int find_scgfx_metadata_in_track(const uint8_t *track, size_t track_len, ScgfxMetadata *meta);
int find_scgfx_metadata_in_midi(const uint8_t *data, size_t size, ScgfxMetadata *meta);
int write_interleaved_format0(FILE *out, const uint8_t *midi_data, size_t midi_size, int fps, int delta_mode, int frame_arg, int argc, char **argv);
int encode_command(int argc, char **argv);
char *make_output_dir(const char *input);
int write_pbm(const char *dir, int frame_no, const uint8_t pixels[16][16]);
int write_ascii(const char *dir, int frame_no, const uint8_t pixels[16][16]);
int gfxwrite_add(GfxWriteList *list, uint32_t tick, uint32_t order, uint8_t offset, const uint8_t *data, uint8_t len, int full);
void gfxwrite_free(GfxWriteList *list);
int compare_gfxwrites(const void *a, const void *b);
int try_add_graphics_sysex(const uint8_t *syx, uint32_t syx_len, uint32_t tick, uint32_t order, GfxWriteList *writes, int *checksum_errors, int *range_errors);
int collect_graphics_writes_from_track(const uint8_t *track, size_t track_len, GfxWriteList *writes, uint32_t *order_counter, int *checksum_errors, int *range_errors);
int collect_graphics_writes_from_midi(const uint8_t *data, size_t size, GfxWriteList *writes, int *checksum_errors, int *range_errors);
int output_frame_file(const char *outdir, int output_ascii, int frame_no, const uint8_t fb[64]);
int unique_graphics_ticks(const GfxWriteList *writes, uint32_t **ticks_out, size_t *count_out);
uint32_t most_common_interval(const uint32_t *ticks, size_t count, int *occurrences_out);
void interval_stats(const uint32_t *ticks, size_t count, uint32_t *min_out, uint32_t *max_out, int *unique_out, uint32_t *gcd_out);
int decode_command(int argc, char **argv);
void describe_dt1_address(uint8_t a0, uint8_t a1, uint8_t a2, const uint8_t *payload, uint32_t payload_len, char *desc, size_t desc_len);
int dump_display_dt1_from_track(const uint8_t *track, size_t track_len, uint16_t track_no, int *shown);
int dump_display_dt1_from_midi(const uint8_t *data, size_t size);
int analyse_command(int argc, char **argv);
int script_event_add_take(ScriptEventList *list, uint32_t tick, uint8_t *data, uint32_t len, uint32_t order);
void script_eventlist_free(ScriptEventList *list);
char *trim_left(char *s);
void trim_right(char *s);
int parse_quoted_string(const char *p, char *out, size_t outlen);
uint8_t *make_dt1_event_data_addr(uint8_t a0, uint8_t a1, uint8_t a2, const uint8_t *payload, uint32_t payload_len, uint32_t *out_len);
int load_script_events(const char *filename, ScriptEventList *script_events);
int script_command(int argc, char **argv);
int write_format0_from_events(const char *output_mid, EventList *events, uint16_t division);
int parse_on_off(const char *s, int *value);
int parse_optional_quoted_or_plain(const char *p, char *out, size_t outlen);
int parse_display_time_value(const char *p, int *value_out);
int load_format0_music_events(const char *input_mid, EventList *events, uint16_t *division_out);
int page_number_to_addr(int page, uint8_t *a0, uint8_t *a1, uint8_t *a2);
int build_command(int argc, char **argv);
int scgfx_main(int argc, char **argv);

#endif
