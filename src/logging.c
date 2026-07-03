#include "scgfx_internal.h"

FILE *g_trace_log = NULL;
int g_verbose = 0;
FILE *g_dump_log = NULL;

void trace_printf(const char *fmt, ...) {
    va_list ap;

    if (!g_trace_log)
        return;

    va_start(ap, fmt);
    vfprintf(g_trace_log, fmt, ap);
    va_end(ap);
}

FILE *open_auto_log(const char *output_name) {
    char path[2048];

    snprintf(path, sizeof(path), "%s.log", output_name);

    FILE *f = fopen(path, "w");
    if (!f)
        perror(path);
    else
        printf("Verbose trace log: %s\n", path);

    return f;
}

void dump_printf(const char *fmt, ...) {
    va_list ap;
    FILE *out = g_dump_log ? g_dump_log : stdout;

    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
}

