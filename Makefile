CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2
CPPFLAGS ?= -Iinclude

SCGFX_SRC = $(wildcard src/*.c)
SCGFX_OBJ = $(SCGFX_SRC:.c=.o)

all: scgfx genscroll

scgfx: $(SCGFX_OBJ)
	$(CC) $(CFLAGS) $(SCGFX_OBJ) -o $@

genscroll: tools/genscroll.c
	$(CC) $(CFLAGS) tools/genscroll.c -o $@

clean:
	rm -f $(SCGFX_OBJ) scgfx genscroll

distclean: clean
	rm -f *.log

.PHONY: all clean distclean