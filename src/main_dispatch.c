#include "scgfx_internal.h"

int scgfx_main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }

    if (strcmp(argv[1], "encode") == 0) return encode_command(argc, argv);
    if (strcmp(argv[1], "decode") == 0) return decode_command(argc, argv);
    if (strcmp(argv[1], "analyse") == 0 || strcmp(argv[1], "analyze") == 0) return analyse_command(argc, argv);
    if (strcmp(argv[1], "compile") == 0) return build_command(argc, argv);

    usage();
    return 1;
}
