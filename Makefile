CC = gcc
CFLAGS ?= -std=c17 -Wall -Wextra

webserver: server.c Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread $(LDFLAGS) -o $@ $< $(LDLIBS)

.PHONY: clean
clean:
	$(RM) webserver
