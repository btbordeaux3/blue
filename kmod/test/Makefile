CC      ?= gcc
CFLAGS  += -Wall -Wextra -I..

espnet_ct: espnet_ct.c ../espnet.h
	$(CC) $(CFLAGS) -o $@ espnet_ct.c

clean:
	rm -f espnet_ct

.PHONY: clean