CFLAGS       = -O -DDEBUG
INC          = -I./include
CC           = gcc

INSTALL      ?= cp -a
PREFIX       ?= /usr/local
INCLUDE_PATH ?= include
LIBRARY_PATH ?= lib

INSTALL_INCLUDE_PATH = $(DESTDIR)$(PREFIX)/$(INCLUDE_PATH)
INSTALL_LIBRARY_PATH = $(DESTDIR)$(PREFIX)/$(LIBRARY_PATH)

BIN     = async_server_demo echo_server_demo
SHARED  = libasync_server.so
SRC     = $(wildcard src/*.c)
OBJ     = $(patsubst src/%.c, obj/%.o, $(SRC))

all: $(BIN) $(SHARED)

async_server_demo: async_server_demo.o async_server.o http_parser.o iso8583_parser.o local_protocol.o rbtree.o
	$(CC) -o $@ $^

echo_server_demo: echo_server_demo.o local_protocol.o
	$(CC) -o $@ $^

libasync_server.so: async_server.o http_parser.o iso8583_parser.o local_protocol.o rbtree.o
	$(CC) -o $@ $^ -shared

$(OBJ): obj/%.o : src/%.c
	$(CC) -c $(CFLAGS) -o $@ $< $(INC)

install:
	mkdir -p $(INSTALL_LIBRARY_PATH) $(INSTALL_INCLUDE_PATH)
	$(INSTALL) include/async_server.h $(INSTALL_INCLUDE_PATH)
	$(INSTALL) $(SHARED) $(INSTALL_LIBRARY_PATH)

uninstall:
	$(RM) $(INSTALL_INCLUDE_PATH)/async_server.h
	$(RM) $(INSTALL_LIBRARY_PATH)/$(SHARED)

clean:
	-rm $(OBJ) $(BIN) $(SHARED)

.PHONY: all clean install uninstall

#.SUFFIXES

vpath %.c  src
vpath %.o  obj
vpath %.h  include
