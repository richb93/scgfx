#include "scgfx_internal.h"
#include <glob.h>



static int build_next_token(char **p, char *out, size_t outlen) {
    size_t n = 0;

    *p = trim_left(*p);

    if (**p == 0)
        return 0;

    while (**p && !isspace((unsigned char)**p)) {
        if (n + 1 < outlen)
            out[n++] = **p;
        (*p)++;
    }

    out[n] = 0;
    *p = trim_left(*p);

    return 1;
}

static int build_next_arg(char **p, char *out, size_t outlen) {
    size_t n = 0;

    *p = trim_left(*p);

    if (**p == 0)
        return 0;

    if (**p == '"') {
        (*p)++;

        while (**p && **p != '"') {
            if (**p == '\\' && (*p)[1]) {
                (*p)++;
                if (n + 1 < outlen)
                    out[n++] = **p;
                (*p)++;
            } else {
                if (n + 1 < outlen)
                    out[n++] = **p;
                (*p)++;
            }
        }

        if (**p != '"')
            return 0;

        (*p)++;
    } else {
        while (**p && !isspace((unsigned char)**p)) {
            if (n + 1 < outlen)
                out[n++] = **p;
            (*p)++;
        }
    }

    out[n] = 0;
    *p = trim_left(*p);

    return 1;
}

static int parse_page_range(const char *text, int *start_out, int *end_out) {
    int a = 0, b = 0;
    char extra = 0;

    if (sscanf(text, "%d-%d%c", &a, &b, &extra) == 2) {
        *start_out = a;
        *end_out = b;
        return 1;
    }

    if (sscanf(text, "%d%c", &a, &extra) == 1) {
        *start_out = a;
        *end_out = a;
        return 1;
    }

    return 0;
}

static int parse_animation_options(char **p,
                                   int project_fps,
                                   int *fps_out,
                                   int *times_out,
                                   int *loop_out) {
    char tok[64];

    *fps_out = project_fps;
    *times_out = 1;
    *loop_out = 0;

    while (**p) {
        if (!build_next_token(p, tok, sizeof(tok)))
            return 0;

        if (strcmp(tok, "fps") == 0) {
            if (!build_next_token(p, tok, sizeof(tok)))
                return 0;

            *fps_out = atoi(tok);

            if (*fps_out <= 0)
                return 0;
        } else if (strcmp(tok, "at") == 0) {
            /*
                Backwards-compatible form:
                    animate pages 1-6 at 10 fps
            */
            if (!build_next_token(p, tok, sizeof(tok)))
                return 0;

            *fps_out = atoi(tok);

            if (*fps_out <= 0)
                return 0;

            if (!build_next_token(p, tok, sizeof(tok)) || strcmp(tok, "fps") != 0)
                return 0;
        } else if (strcmp(tok, "repeat") == 0) {
            if (!build_next_token(p, tok, sizeof(tok)))
                return 0;

            *times_out = atoi(tok);

            if (*times_out <= 0)
                return 0;
        } else if (strcmp(tok, "for") == 0) {
            /*
                Backwards-compatible form:
                    for 15 times
            */
            if (!build_next_token(p, tok, sizeof(tok)))
                return 0;

            *times_out = atoi(tok);

            if (*times_out <= 0)
                return 0;

            if (!build_next_token(p, tok, sizeof(tok)) || strcmp(tok, "times") != 0)
                return 0;
        } else if (strcmp(tok, "loop") == 0) {
            *loop_out = 1;
        } else {
            return 0;
        }
    }

    return 1;
}


static int has_glob_magic(const char *s) {
    while (*s) {
        if (*s == '*' || *s == '?' || *s == '[')
            return 1;
        s++;
    }

    return 0;
}

int page_number_to_addr(int page, uint8_t *a0, uint8_t *a1, uint8_t *a2) {
    /*
        SC-88 display page dot data.

        The SC-88 manual refers to display pages 1-10.

        Dot-data storage is arranged as two pages per address block:

            Page 1  -> 10 01 00
            Page 2  -> 10 01 40

            Page 3  -> 10 02 00
            Page 4  -> 10 02 40

            Page 5  -> 10 03 00
            Page 6  -> 10 03 40

            Page 7  -> 10 04 00
            Page 8  -> 10 04 40

            Page 9  -> 10 05 00
            Page 10 -> 10 05 40

        Selecting which preloaded page is shown is done separately by writing
        the user-facing page number 1-10 directly to address 10 20 00.

        Note: Page 1 is the immediate display page, so loading Page 1 may also
        visibly update the screen straight away.
    */
    if (page < 1 || page > 10)
        return 0;

    *a0 = 0x10;
    *a1 = (uint8_t)((page + 1) / 2);
    *a2 = (page % 2) ? 0x00 : 0x40;
    return 1;
}

int build_command(int argc, char **argv) {
    int verbose = 0;
    const char *project_file = NULL;

    if (argc == 3) {
        project_file = argv[2];
    } else if (argc == 4 &&
               (strcmp(argv[2], "-v") == 0 || strcmp(argv[2], "-verbose") == 0)) {
        verbose = 1;
        project_file = argv[3];
    } else if (argc == 4 && strcmp(argv[2], "-vv") == 0) {
        verbose = 2;
        project_file = argv[3];
    } else {
        fprintf(stderr,
            "Usage:\n"
            "  scgfx compile [-v|-vv|-verbose] project.scg\n\n"

            "Script format (.scg):\n\n"

            "Project settings:\n"
            "  output \"out.mid\"                 Set output MIDI filename\n"
            "  input \"song.mid\"                 Optional Format 0 input MIDI\n"
            "  fps N                           Set project animation FPS\n"
            "  delta on|off                    Enable/disable SC-88 delta writes\n"
            "  songlength TICKS                Optional end tick for looped animations\n\n"

            "Resources:\n"
            "  page N \"image.pbm\"             Load one image into SC-88 page N\n"
            "  page N \"frames/*.pbm\"          Load wildcard matches into pages N-10\n"
            "                                  Fails if too many files match\n"
            "  animation NAME \"frames/*.pbm\"  Define named image animation\n"
            "  pageanim NAME 1-10              Define named page animation\n\n"

            "Timeline:\n"
            "  at TICK text \"message\"         Display up to 32 ASCII chars\n"
            "  at TICK page N                  Display page 0-10\n"
            "  at TICK time N                  Display time raw value 0-15\n"
            "                                  Also accepts 2.88s or 1500ms\n"
            "                                  1 unit = 0.48s; default 6 = 2.88s\n"
            "  at TICK image \"image.pbm\"      Immediate image write\n"
            "  at TICK animate pages 1-6       Page animation using project FPS\n"
            "  at TICK animate pages 1-6 fps N repeat N\n"
            "  at TICK animate images \"frames/*.pbm\"\n"
            "  at TICK animate images \"frames/*.pbm\" fps N repeat N\n"
            "  at TICK play NAME [fps N] [repeat N|loop]\n\n"

            "Example:\n"
            "  output \"demo.mid\"\n"
            "  input \"song.mid\"\n"
            "  fps 10\n"
            "  delta on\n"
            "  page 1 \"logo.pbm\"\n"
            "  page 2 \"intro/*.pbm\"\n"
            "  animation spinner \"spinner/*.pbm\"\n"
            "  pageanim intro 2-10\n"
            "  at 0 page 1\n"
            "  at 0 text \"SCGFX Demo\"\n"
            "  at 480 play intro\n"
            "  at 2000 play spinner repeat 3\n");
        return 1;
    }

    FILE *f = fopen(project_file, "r");
    if (!f) {
        perror(project_file);
        return 1;
    }

    char input_mid[1024] = "";
    char output_mid[1024] = "";
    int fps = DEFAULT_FPS;
    int delta_mode = 0;
    uint32_t song_length = 0;
    uint16_t division = TPQN;

    typedef struct {
        uint32_t tick;
        char command[32];
        char arg[1024];
        int number;
        int end_number;
        int anim_fps;
        int times;
        int loop;
    } ProjectCommand;

    typedef struct {
        int page;
        char filename[1024];
        int line_no;
    } PageDef;

    typedef struct {
        char name[64];
        char type[16];
        char arg[1024];
        int start_page;
        int end_page;
    } AnimationDef;

    ProjectCommand *cmds = NULL;
    size_t cmd_count = 0, cmd_cap = 0;

    PageDef *pages = NULL;
    size_t page_count = 0, page_cap = 0;

    AnimationDef *anims = NULL;
    size_t anim_count = 0, anim_cap = 0;

    char line[2048];
    int line_no = 0;

    while (fgets(line, sizeof(line), f)) {
        line_no++;
        char *p = trim_left(line);
        trim_right(p);

        if (*p == 0 || *p == '#')
            continue;

        if (strncmp(p, "input", 5) == 0 && isspace((unsigned char)p[5])) {
            if (!parse_optional_quoted_or_plain(p + 5, input_mid, sizeof(input_mid))) {
                fprintf(stderr, "%s:%d: bad input line\n", project_file, line_no);
                fclose(f);
                free(cmds);
                return 1;
            }
        } else if (strncmp(p, "output", 6) == 0 && isspace((unsigned char)p[6])) {
            if (!parse_optional_quoted_or_plain(p + 6, output_mid, sizeof(output_mid))) {
                fprintf(stderr, "%s:%d: bad output line\n", project_file, line_no);
                fclose(f);
                free(cmds);
                return 1;
            }
        } else if (strncmp(p, "fps", 3) == 0 && isspace((unsigned char)p[3])) {
            fps = atoi(trim_left(p + 3));
            if (fps <= 0) {
                fprintf(stderr, "%s:%d: fps must be greater than zero\n", project_file, line_no);
                fclose(f);
                free(cmds);
                return 1;
            }
        } else if (strncmp(p, "delta", 5) == 0 && isspace((unsigned char)p[5])) {
            if (!parse_on_off(trim_left(p + 5), &delta_mode)) {
                fprintf(stderr, "%s:%d: delta must be on or off\n", project_file, line_no);
                fclose(f);
                free(cmds);
                free(pages);
                return 1;
            }
        } else if (strncmp(p, "songlength", 10) == 0 && isspace((unsigned char)p[10])) {
            song_length = (uint32_t)strtoul(trim_left(p + 10), NULL, 10);

            if (song_length == 0) {
                fprintf(stderr, "%s:%d: songlength must be greater than zero ticks\n",
                        project_file, line_no);
                fclose(f);
                free(cmds);
                free(pages);
                free(anims);
                return 1;
            }
        } else if (strncmp(p, "animation", 9) == 0 && isspace((unsigned char)p[9])) {
            char name[64];
            char pattern[1024];

            p = trim_left(p + 9);

            if (!build_next_token(&p, name, sizeof(name)) ||
                !build_next_arg(&p, pattern, sizeof(pattern))) {
                fprintf(stderr, "%s:%d: expected: animation name \"frames/*.pbm\"\n",
                        project_file, line_no);
                fclose(f);
                free(cmds);
                free(pages);
                free(anims);
                return 1;
            }

            if (anim_count == anim_cap) {
                size_t new_cap = anim_cap ? anim_cap * 2 : 16;
                AnimationDef *new_anims = (AnimationDef *)realloc(anims,
                                                                  new_cap * sizeof(AnimationDef));
                if (!new_anims) {
                    fclose(f);
                    free(cmds);
                    free(pages);
                    free(anims);
                    return 1;
                }

                anims = new_anims;
                anim_cap = new_cap;
            }

            memset(&anims[anim_count], 0, sizeof(anims[anim_count]));
            strncpy(anims[anim_count].name, name, sizeof(anims[anim_count].name) - 1);
            strncpy(anims[anim_count].type, "images", sizeof(anims[anim_count].type) - 1);
            strncpy(anims[anim_count].arg, pattern, sizeof(anims[anim_count].arg) - 1);
            anim_count++;
        } else if (strncmp(p, "pageanim", 8) == 0 && isspace((unsigned char)p[8])) {
            char name[64];
            char range[64];
            int start_page = 0, end_page = 0;

            p = trim_left(p + 8);

            if (!build_next_token(&p, name, sizeof(name)) ||
                !build_next_token(&p, range, sizeof(range)) ||
                !parse_page_range(range, &start_page, &end_page)) {
                fprintf(stderr, "%s:%d: expected: pageanim name 1-6\n",
                        project_file, line_no);
                fclose(f);
                free(cmds);
                free(pages);
                free(anims);
                return 1;
            }

            if (anim_count == anim_cap) {
                size_t new_cap = anim_cap ? anim_cap * 2 : 16;
                AnimationDef *new_anims = (AnimationDef *)realloc(anims,
                                                                  new_cap * sizeof(AnimationDef));
                if (!new_anims) {
                    fclose(f);
                    free(cmds);
                    free(pages);
                    free(anims);
                    return 1;
                }

                anims = new_anims;
                anim_cap = new_cap;
            }

            memset(&anims[anim_count], 0, sizeof(anims[anim_count]));
            strncpy(anims[anim_count].name, name, sizeof(anims[anim_count].name) - 1);
            strncpy(anims[anim_count].type, "pages", sizeof(anims[anim_count].type) - 1);
            anims[anim_count].start_page = start_page;
            anims[anim_count].end_page = end_page;
            anim_count++;
        } else if (strncmp(p, "page", 4) == 0 && isspace((unsigned char)p[4])) {
            /*
                Top-level page definition, no timing:

                    page 1 "frame0001.pbm"

                This preloads dot data into SC-88 display page memory at tick 0.
                Timed page display is still written as:

                    at 480 page 1
            */
            p = trim_left(p + 4);
            char *endp = NULL;
            unsigned long page_ul = strtoul(p, &endp, 10);

            if (endp == p || page_ul < 1 || page_ul > 10) {
                fprintf(stderr, "%s:%d: page definition must use page 1-10\n",
                        project_file, line_no);
                fclose(f);
                free(cmds);
                free(pages);
                return 1;
            }

            char filename[1024];
            if (!parse_optional_quoted_or_plain(endp, filename, sizeof(filename))) {
                fprintf(stderr, "%s:%d: expected filename for page definition\n",
                        project_file, line_no);
                fclose(f);
                free(cmds);
                free(pages);
                return 1;
            }

            if (strpbrk(filename, "*?[") != NULL) {
                glob_t matches;
                int glob_result;
                int available_pages = 10 - (int)page_ul + 1;

                memset(&matches, 0, sizeof(matches));
                glob_result = glob(filename, 0, NULL, &matches);

                if (glob_result != 0 || matches.gl_pathc == 0) {
                    fprintf(stderr,
                            "%s:%d: page wildcard matched no files: %s\n",
                            project_file, line_no, filename);
                    globfree(&matches);
                    fclose(f);
                    free(cmds);
                    free(pages);
                    free(anims);
                    return 1;
                }

                if ((int)matches.gl_pathc > available_pages) {
                    fprintf(stderr,
                            "%s:%d: page wildcard \"%s\" matched %zu images, "
                            "but only %d page%s available from page %lu to 10.\n",
                            project_file, line_no, filename, matches.gl_pathc,
                            available_pages, available_pages == 1 ? " is" : "s are",
                            page_ul);
                    fprintf(stderr,
                            "Reduce the number of images or start at an earlier page.\n");
                    globfree(&matches);
                    fclose(f);
                    free(cmds);
                    free(pages);
                    free(anims);
                    return 1;
                }

                for (size_t gi = 0; gi < matches.gl_pathc; gi++) {
                    if (page_count == page_cap) {
                        size_t new_cap = page_cap ? page_cap * 2 : 16;
                        PageDef *new_pages = (PageDef *)realloc(pages, new_cap * sizeof(PageDef));
                        if (!new_pages) {
                            globfree(&matches);
                            fclose(f);
                            free(cmds);
                            free(pages);
                            free(anims);
                            return 1;
                        }
                        pages = new_pages;
                        page_cap = new_cap;
                    }

                    pages[page_count].page = (int)page_ul + (int)gi;
                    pages[page_count].line_no = line_no;
                    strncpy(pages[page_count].filename, matches.gl_pathv[gi],
                            sizeof(pages[page_count].filename) - 1);
                    pages[page_count].filename[sizeof(pages[page_count].filename) - 1] = 0;
                    page_count++;
                }

                globfree(&matches);
            } else {
                if (page_count == page_cap) {
                    size_t new_cap = page_cap ? page_cap * 2 : 16;
                    PageDef *new_pages = (PageDef *)realloc(pages, new_cap * sizeof(PageDef));
                    if (!new_pages) {
                        fclose(f);
                        free(cmds);
                        free(pages);
                        free(anims);
                        return 1;
                    }
                    pages = new_pages;
                    page_cap = new_cap;
                }

                pages[page_count].page = (int)page_ul;
                pages[page_count].line_no = line_no;
                strncpy(pages[page_count].filename, filename, sizeof(pages[page_count].filename) - 1);
                pages[page_count].filename[sizeof(pages[page_count].filename) - 1] = 0;
                page_count++;
            }
        } else if (strncmp(p, "at", 2) == 0 && isspace((unsigned char)p[2])) {
            p = trim_left(p + 2);
            char *endp = NULL;
            unsigned long tick_ul = strtoul(p, &endp, 10);

            if (endp == p) {
                fprintf(stderr, "%s:%d: expected tick after 'at'\n", project_file, line_no);
                fclose(f);
                free(cmds);
                return 1;
            }

            p = trim_left(endp);

            char command[32];
            int ci = 0;
            while (*p && !isspace((unsigned char)*p)) {
                if (ci < (int)sizeof(command) - 1)
                    command[ci++] = *p;
                p++;
            }
            command[ci] = 0;
            p = trim_left(p);

            if (cmd_count == cmd_cap) {
                size_t new_cap = cmd_cap ? cmd_cap * 2 : 128;
                ProjectCommand *new_cmds = (ProjectCommand *)realloc(cmds, new_cap * sizeof(ProjectCommand));
                if (!new_cmds) {
                    fclose(f);
                    free(cmds);
                    return 1;
                }
                cmds = new_cmds;
                cmd_cap = new_cap;
            }

            ProjectCommand *c = &cmds[cmd_count++];
            memset(c, 0, sizeof(*c));
            c->tick = (uint32_t)tick_ul;
            strncpy(c->command, command, sizeof(c->command) - 1);

            if (strcmp(command, "text") == 0 || strcmp(command, "image") == 0) {
                if (!parse_optional_quoted_or_plain(p, c->arg, sizeof(c->arg))) {
                    fprintf(stderr, "%s:%d: expected argument for %s\n", project_file, line_no, command);
                    fclose(f);
                    free(cmds);
                    return 1;
                }
            } else if (strcmp(command, "page") == 0) {
                c->number = atoi(p);
            } else if (strcmp(command, "time") == 0) {
                if (!parse_optional_quoted_or_plain(p, c->arg, sizeof(c->arg))) {
                    fprintf(stderr, "%s:%d: expected time value for time command\n", project_file, line_no);
                    fclose(f);
                    free(cmds);
                    return 1;
                }
            } else if (strcmp(command, "animate") == 0) {
                char anim_type[64];

                if (!build_next_token(&p, anim_type, sizeof(anim_type))) {
                    fprintf(stderr, "%s:%d: expected 'pages' or 'images' after animate\n",
                            project_file, line_no);
                    fclose(f);
                    free(cmds);
                    free(pages);
                    free(anims);
                    return 1;
                }

                if (strcmp(anim_type, "pages") == 0) {
                    char range[64];

                    strncpy(c->command, "animate_pages", sizeof(c->command) - 1);

                    if (!build_next_token(&p, range, sizeof(range)) ||
                        !parse_page_range(range, &c->number, &c->end_number)) {
                        fprintf(stderr, "%s:%d: expected page range such as 1-6\n",
                                project_file, line_no);
                        fclose(f);
                        free(cmds);
                        free(pages);
                        free(anims);
                        return 1;
                    }

                    if (!parse_animation_options(&p, fps, &c->anim_fps, &c->times, &c->loop)) {
                        fprintf(stderr,
                                "%s:%d: expected syntax: at <tick> animate pages 1-6 [fps 10] [repeat 15|loop]\n",
                                project_file, line_no);
                        fclose(f);
                        free(cmds);
                        free(pages);
                        free(anims);
                        return 1;
                    }
                } else if (strcmp(anim_type, "images") == 0) {
                    strncpy(c->command, "animate_images", sizeof(c->command) - 1);

                    if (!build_next_arg(&p, c->arg, sizeof(c->arg))) {
                        fprintf(stderr, "%s:%d: expected image wildcard/path after animate images\n",
                                project_file, line_no);
                        fclose(f);
                        free(cmds);
                        free(pages);
                        free(anims);
                        return 1;
                    }

                    if (!parse_animation_options(&p, fps, &c->anim_fps, &c->times, &c->loop)) {
                        fprintf(stderr,
                                "%s:%d: expected syntax: at <tick> animate images \"frames/frame*.pbm\" [fps 10] [repeat 3|loop]\n",
                                project_file, line_no);
                        fclose(f);
                        free(cmds);
                        free(pages);
                        free(anims);
                        return 1;
                    }
                } else {
                    fprintf(stderr, "%s:%d: unknown animate type '%s'\n",
                            project_file, line_no, anim_type);
                    fclose(f);
                    free(cmds);
                    free(pages);
                    free(anims);
                    return 1;
                }
            } else if (strcmp(command, "play") == 0) {
                char name[64];
                AnimationDef *found = NULL;

                if (!build_next_token(&p, name, sizeof(name))) {
                    fprintf(stderr, "%s:%d: expected animation name after play\n",
                            project_file, line_no);
                    fclose(f);
                    free(cmds);
                    free(pages);
                    free(anims);
                    return 1;
                }

                for (size_t ai = 0; ai < anim_count; ai++) {
                    if (strcmp(anims[ai].name, name) == 0) {
                        found = &anims[ai];
                        break;
                    }
                }

                if (!found) {
                    fprintf(stderr, "%s:%d: unknown animation '%s'\n",
                            project_file, line_no, name);
                    fclose(f);
                    free(cmds);
                    free(pages);
                    free(anims);
                    return 1;
                }

                if (strcmp(found->type, "pages") == 0) {
                    strncpy(c->command, "animate_pages", sizeof(c->command) - 1);
                    c->number = found->start_page;
                    c->end_number = found->end_page;
                } else {
                    strncpy(c->command, "animate_images", sizeof(c->command) - 1);
                    strncpy(c->arg, found->arg, sizeof(c->arg) - 1);
                }

                if (!parse_animation_options(&p, fps, &c->anim_fps, &c->times, &c->loop)) {
                    fprintf(stderr,
                            "%s:%d: expected syntax: at <tick> play %s [fps 10] [repeat 3|loop]\n",
                            project_file, line_no, name);
                    fclose(f);
                    free(cmds);
                    free(pages);
                    free(anims);
                    return 1;
                }
            } else {
                fprintf(stderr, "%s:%d: unknown command '%s'\n", project_file, line_no, command);
                fclose(f);
                free(cmds);
                return 1;
            }
        } else {
            fprintf(stderr, "%s:%d: unknown project line\n", project_file, line_no);
            fclose(f);
            free(cmds);
            return 1;
        }
    }

    fclose(f);

    if (output_mid[0] == 0) {
        fprintf(stderr, "%s: project must specify output \"file.mid\"\n", project_file);
        free(cmds);
        return 1;
    }

    EventList events;
    memset(&events, 0, sizeof(events));

    if (input_mid[0]) {
        if (!load_format0_music_events(input_mid, &events, &division)) {
            eventlist_free(&events);
            free(cmds);
            free(pages);
            free(anims);
            return 1;
        }

        if (song_length == 0) {
            for (size_t ei = 0; ei < events.count; ei++) {
                if (events.events[ei].tick > song_length)
                    song_length = events.events[ei].tick;
            }
        }
    } else {
        division = TPQN;
    }

    uint32_t order = 2000000000u;
    uint8_t prev_fb[64];
    memset(prev_fb, 0, 64);
    int have_prev = 0;
    int inserted = 0;
    DeltaStats stats;
    memset(&stats, 0, sizeof(stats));

    g_verbose = verbose;

    if (verbose >= 2)
        g_trace_log = open_auto_log(output_mid);

    if (verbose) {
        printf("Compile mode\n");
        printf("  Project:     %s\n", project_file);
        printf("  Output:      %s\n", output_mid);
        printf("  Input MIDI:  %s\n", input_mid[0] ? input_mid : "(none)");
        printf("  FPS:         %d\n", fps);
        printf("  Delta:       %s\n", delta_mode ? "on" : "off");
    }

    trace_printf("scgfx build trace\nProject: %s\nOutput: %s\nInput MIDI: %s\nFPS: %d\nDelta: %s\n\n",
                 project_file, output_mid, input_mid[0] ? input_mid : "(none)",
                 fps, delta_mode ? "on" : "off");

    if (delta_mode)
        fprintf(stderr, "Warning: project delta mode is intended for SC-88 or later. It is not SC-55 compatible.\n");

    /*
        Preload SC-88 display pages at tick 0.

        These are definitions, not timed display events. A later timed command
        like "at 480 page 2" selects which preloaded page is shown.

        A page definition can be a single file:

            page 3 "logo.pbm"

        or a wildcard:

            page 3 "intro/frame*.pbm"

        Wildcard matches are loaded into consecutive pages starting at the
        supplied page number. The match count must fit within pages N-10.
    */
    for (size_t i = 0; i < page_count; i++) {
        glob_t page_matches;
        const char **filenames = NULL;
        const char *single_filename = NULL;
        size_t file_count = 0;
        int globbed = has_glob_magic(pages[i].filename);

        if (pages[i].page < 1 || pages[i].page > 10) {
            fprintf(stderr, "%s:%d: page definition must use page 1-10\n",
                    project_file, pages[i].line_no);
            eventlist_free(&events);
            free(cmds);
            free(pages);
            free(anims);
            return 1;
        }

        memset(&page_matches, 0, sizeof(page_matches));

        if (globbed) {
            int glob_result = glob(pages[i].filename, 0, NULL, &page_matches);

            if (glob_result != 0 || page_matches.gl_pathc == 0) {
                fprintf(stderr, "%s:%d: page wildcard matched no files: %s\n",
                        project_file, pages[i].line_no, pages[i].filename);
                globfree(&page_matches);
                eventlist_free(&events);
                free(cmds);
                free(pages);
                free(anims);
                return 1;
            }

            file_count = page_matches.gl_pathc;
            filenames = (const char **)page_matches.gl_pathv;

            if ((int)file_count > (11 - pages[i].page)) {
                fprintf(stderr,
                        "%s:%d: page wildcard \"%s\" matched %zu file%s, "
                        "but only %d page%s available from page %d to page 10\n",
                        project_file,
                        pages[i].line_no,
                        pages[i].filename,
                        file_count,
                        file_count == 1 ? "" : "s",
                        11 - pages[i].page,
                        (11 - pages[i].page) == 1 ? "" : "s",
                        pages[i].page);
                globfree(&page_matches);
                eventlist_free(&events);
                free(cmds);
                free(pages);
                free(anims);
                return 1;
            }
        } else {
            single_filename = pages[i].filename;
            filenames = &single_filename;
            file_count = 1;
        }

        for (size_t mi = 0; mi < file_count; mi++) {
            uint8_t pixels[16][16], fb[64];
            uint8_t a0, a1, a2;
            uint8_t *ev = NULL;
            uint32_t evlen = 0;
            int page_number = pages[i].page + (int)mi;

            if (!page_number_to_addr(page_number, &a0, &a1, &a2)) {
                fprintf(stderr, "%s:%d: page definition must use page 1-10\n",
                        project_file, pages[i].line_no);
                if (globbed)
                    globfree(&page_matches);
                eventlist_free(&events);
                free(cmds);
                free(pages);
                free(anims);
                return 1;
            }

            if (verbose)
                printf("Tick 0: Page %d <- %s -> DT1 10 %02X %02X\n",
                       page_number, filenames[mi], a1, a2);

            trace_printf("Tick 0: preload page %d from %s -> DT1 10 %02X %02X len=64\n",
                         page_number, filenames[mi], a1, a2);

            if (!load_frame(filenames[mi], pixels)) {
                if (globbed)
                    globfree(&page_matches);
                eventlist_free(&events);
                free(cmds);
                free(pages);
                free(anims);
                return 1;
            }

            build_framebuffer(pixels, fb);

            ev = make_dt1_event_data_addr(a0, a1, a2, fb, 64, &evlen);

            if (!ev || !eventlist_add_take(&events, 0, ev, evlen, order++)) {
                free(ev);
                if (globbed)
                    globfree(&page_matches);
                eventlist_free(&events);
                free(cmds);
                free(pages);
                free(anims);
                return 1;
            }

            inserted++;
        }

        if (globbed)
            globfree(&page_matches);
    }

    for (size_t i = 0; i < cmd_count; i++) {
        ProjectCommand *c = &cmds[i];
        uint8_t *ev = NULL;
        uint32_t evlen = 0;

        if (verbose) {
            if (strcmp(c->command, "text") == 0)
                printf("Tick %u: text \"%s\" -> DT1 10 00 00\n", c->tick, c->arg);
            else if (strcmp(c->command, "page") == 0)
                printf("Tick %u: display page %d -> DT1 10 20 00\n", c->tick, c->number);
            else if (strcmp(c->command, "time") == 0)
                printf("Tick %u: display time %s -> DT1 10 20 01\n", c->tick, c->arg);
            else if (strcmp(c->command, "image") == 0)
                printf("Tick %u: immediate image %s -> DT1 10 01 00\n", c->tick, c->arg);
            else if (strcmp(c->command, "animate_pages") == 0)
                printf("Tick %u: animate pages %d-%d at %d fps%s%d times\n",
                       c->tick, c->number, c->end_number, c->anim_fps,
                       c->loop ? " loop, " : " repeat ", c->times);
            else if (strcmp(c->command, "animate_images") == 0)
                printf("Tick %u: animate images %s at %d fps%s%d times\n",
                       c->tick, c->arg, c->anim_fps,
                       c->loop ? " loop, " : " repeat ", c->times);
        }
        if (strcmp(c->command, "text") == 0)
            trace_printf("Tick %u: text \"%s\" -> DT1 10 00 00\n", c->tick, c->arg);
        else if (strcmp(c->command, "page") == 0)
            trace_printf("Tick %u: display page %d -> DT1 10 20 00\n", c->tick, c->number);
        else if (strcmp(c->command, "time") == 0)
            trace_printf("Tick %u: display time %s -> DT1 10 20 01\n", c->tick, c->arg);
        else if (strcmp(c->command, "image") == 0)
            trace_printf("Tick %u: immediate image %s -> DT1 10 01 00\n", c->tick, c->arg);
        else if (strcmp(c->command, "animate_pages") == 0)
            trace_printf("Tick %u: animate pages %d-%d at %d fps%s%d times\n",
                       c->tick, c->number, c->end_number, c->anim_fps,
                       c->loop ? " loop, " : " repeat ", c->times);
        else if (strcmp(c->command, "animate_images") == 0)
            trace_printf("Tick %u: animate images %s at %d fps%s%d times\n",
                       c->tick, c->arg, c->anim_fps,
                       c->loop ? " loop, " : " repeat ", c->times);

        if (strcmp(c->command, "text") == 0) {
            size_t len = strlen(c->arg);
            if (len > 32) {
                fprintf(stderr, "%s: text at tick %u is %zu chars; truncating to 32\n",
                        project_file, c->tick, len);
                len = 32;
            }
            ev = make_dt1_event_data_addr(0x10, 0x00, 0x00,
                                          (const uint8_t *)c->arg,
                                          (uint32_t)len,
                                          &evlen);
            if (!ev || !eventlist_add_take(&events, c->tick, ev, evlen, order++)) {
                free(ev);
                eventlist_free(&events);
                free(cmds);
                return 1;
            }
            inserted++;
        } else if (strcmp(c->command, "page") == 0) {
            if (c->number < 0 || c->number > 10) {
                fprintf(stderr, "%s: page at tick %u must be 0-10\n", project_file, c->tick);
                eventlist_free(&events);
                free(cmds);
                return 1;
            }
            uint8_t v = (uint8_t)c->number;
            ev = make_dt1_event_data_addr(0x10, 0x20, 0x00, &v, 1, &evlen);
            if (!ev || !eventlist_add_take(&events, c->tick, ev, evlen, order++)) {
                free(ev);
                eventlist_free(&events);
                free(cmds);
                return 1;
            }
            inserted++;
        } else if (strcmp(c->command, "time") == 0) {
            int display_time = 0;

            if (!parse_display_time_value(c->arg, &display_time)) {
                fprintf(stderr,
                        "%s: time at tick %u must be 0-15, or seconds/ms such as 2.88s or 1500ms\n",
                        project_file, c->tick);
                eventlist_free(&events);
                free(cmds);
                return 1;
            }

            uint8_t v = (uint8_t)display_time;
            ev = make_dt1_event_data_addr(0x10, 0x20, 0x01, &v, 1, &evlen);
            if (!ev || !eventlist_add_take(&events, c->tick, ev, evlen, order++)) {
                free(ev);
                eventlist_free(&events);
                free(cmds);
                return 1;
            }
            inserted++;
        } else if (strcmp(c->command, "animate_pages") == 0) {
            int start_page = c->number;
            int end_page = c->end_number;
            int step = (end_page >= start_page) ? 1 : -1;
            int page_count = (end_page >= start_page) ?
                (end_page - start_page + 1) :
                (start_page - end_page + 1);
            uint32_t ticks_per_anim_frame = (division * 2) / (uint32_t)c->anim_fps;

            if (ticks_per_anim_frame < 1)
                ticks_per_anim_frame = 1;

            if (c->loop) {
                if (song_length <= c->tick) {
                    fprintf(stderr,
                            "%s: animate pages loop at tick %u needs input MIDI length or songlength setting\n",
                            project_file, c->tick);
                    eventlist_free(&events);
                    free(cmds);
                    free(pages);
                    free(anims);
                    return 1;
                }

                c->times = (int)((song_length - c->tick + (page_count * ticks_per_anim_frame) - 1) /
                                 (page_count * ticks_per_anim_frame));

                if (c->times < 1)
                    c->times = 1;
            }

            if (start_page < 1 || start_page > 10 ||
                end_page < 1 || end_page > 10) {
                fprintf(stderr, "%s: animate pages at tick %u must use pages 1-10\n",
                        project_file, c->tick);
                eventlist_free(&events);
                free(cmds);
                return 1;
            }

            for (int rep = 0; rep < c->times; rep++) {
                for (int idx = 0; idx < page_count; idx++) {
                    int page = start_page + (idx * step);
                    uint32_t tick = c->tick +
                        (uint32_t)((rep * page_count) + idx) * ticks_per_anim_frame;
                    uint8_t v = (uint8_t)page;

                    ev = make_dt1_event_data_addr(0x10, 0x20, 0x00, &v, 1, &evlen);

                    if (!ev || !eventlist_add_take(&events, tick, ev, evlen, order++)) {
                        free(ev);
                        eventlist_free(&events);
                        free(cmds);
                        return 1;
                    }

                    inserted++;
                }
            }
        } else if (strcmp(c->command, "animate_images") == 0) {
            glob_t matches;
            uint32_t ticks_per_anim_frame = (division * 2) / (uint32_t)c->anim_fps;
            int glob_result;

            if (ticks_per_anim_frame < 1)
                ticks_per_anim_frame = 1;

            memset(&matches, 0, sizeof(matches));
            glob_result = glob(c->arg, 0, NULL, &matches);

            if (glob_result != 0 || matches.gl_pathc == 0) {
                fprintf(stderr, "%s: animate images at tick %u matched no files: %s\n",
                        project_file, c->tick, c->arg);
                globfree(&matches);
                eventlist_free(&events);
                free(cmds);
                return 1;
            }

            if (c->loop) {
                uint32_t cycle_ticks;

                if (song_length <= c->tick) {
                    fprintf(stderr,
                            "%s: animate images loop at tick %u needs input MIDI length or songlength setting\n",
                            project_file, c->tick);
                    globfree(&matches);
                    eventlist_free(&events);
                    free(cmds);
                    free(pages);
                    free(anims);
                    return 1;
                }

                cycle_ticks = (uint32_t)matches.gl_pathc * ticks_per_anim_frame;
                c->times = (int)((song_length - c->tick + cycle_ticks - 1) / cycle_ticks);

                if (c->times < 1)
                    c->times = 1;
            }

            for (int rep = 0; rep < c->times; rep++) {
                for (size_t idx = 0; idx < matches.gl_pathc; idx++) {
                    uint8_t pixels[16][16], fb[64];
                    uint32_t tick = c->tick +
                        (uint32_t)((rep * matches.gl_pathc) + idx) * ticks_per_anim_frame;

                    if (!load_frame(matches.gl_pathv[idx], pixels)) {
                        globfree(&matches);
                        eventlist_free(&events);
                        free(cmds);
                        return 1;
                    }

                    build_framebuffer(pixels, fb);

                    if (delta_mode) {
                        stats.frames_total++;
                        stats.full_equivalent_bytes += FULL_FRAME_EVENT_BYTES;
                    }

                    if (!delta_mode || !have_prev) {
                        ev = make_sc55_event_data(fb, &evlen);

                        if (!ev || !eventlist_add_take(&events, tick, ev, evlen, order++)) {
                            free(ev);
                            globfree(&matches);
                            eventlist_free(&events);
                            free(cmds);
                            return 1;
                        }

                        if (delta_mode) {
                            stats.full_frames++;
                            stats.full_packets++;
                            stats.actual_graphics_bytes += evlen;
                        }
                    } else {
                        int runs = add_delta_events(&events, tick, prev_fb, fb,
                                                    &order, delta_mode ? &stats : NULL);

                        if (runs < 0) {
                            globfree(&matches);
                            eventlist_free(&events);
                            free(cmds);
                            return 1;
                        }
                    }

                    memcpy(prev_fb, fb, 64);
                    have_prev = 1;
                    inserted++;
                }
            }

            globfree(&matches);
        } else if (strcmp(c->command, "image") == 0) {
            uint8_t pixels[16][16], fb[64];
            if (!load_frame(c->arg, pixels)) {
                eventlist_free(&events);
                free(cmds);
                return 1;
            }
            build_framebuffer(pixels, fb);

            if (delta_mode) {
                stats.frames_total++;
                stats.full_equivalent_bytes += FULL_FRAME_EVENT_BYTES;
            }

            if (!delta_mode || !have_prev) {
                ev = make_sc55_event_data(fb, &evlen);
                if (!ev || !eventlist_add_take(&events, c->tick, ev, evlen, order++)) {
                    free(ev);
                    eventlist_free(&events);
                    free(cmds);
                    return 1;
                }
                if (delta_mode) {
                    stats.full_frames++;
                    stats.full_packets++;
                    stats.actual_graphics_bytes += evlen;
                }
            } else {
                int runs = add_delta_events(&events, c->tick, prev_fb, fb, &order, delta_mode ? &stats : NULL);
                if (runs < 0) {
                    eventlist_free(&events);
                    free(cmds);
                    return 1;
                }
            }

            memcpy(prev_fb, fb, 64);
            have_prev = 1;
            inserted++;
        }
    }

    if (!write_format0_from_events(output_mid, &events, division)) {
        eventlist_free(&events);
        free(cmds);
        return 1;
    }

    eventlist_free(&events);
    free(cmds);
    free(pages);
    free(anims);

    printf("Generated %s\n", output_mid);
    printf("Inserted %d project event(s)\n", inserted);
    if (delta_mode)
        print_delta_stats(&stats);

    trace_printf("\nInserted project events: %d\n", inserted);
    if (g_trace_log) { fclose(g_trace_log); g_trace_log = NULL; }

    return 0;
}

