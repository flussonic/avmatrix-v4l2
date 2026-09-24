CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter

all: hwav

hwav: hwav.c ../include/sdi_av.h ../include/hwav.h
	$(CC) $(CFLAGS) -o $@ hwav.c -lm

clean:
	rm -f hwav

.PHONY: all clean
