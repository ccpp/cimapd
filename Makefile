CFLAGS=-Wall

default: cimapd
.PHONY: default

cimapd: main.o
	$(CC) $(LDFLAGS) $^ -o $@

