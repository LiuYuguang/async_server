CFLAGS       = -O3 -Wall
INC          = -I./include
CC           = gcc
LIBS         = 
TRAGET  = server echo
SRC     = $(wildcard src/*.c)
OBJ     = $(patsubst src/%.c, obj/%.o, $(SRC))

all: obj $(TRAGET)

server: server.o async_server.o http_parser.o rbtree.o
	$(CC) -o $@ $^ $(LIBS)

echo: echo.o async_server.o http_parser.o  rbtree.o
	$(CC) -o $@ $^ $(LIBS)

$(OBJ): obj/%.o : src/%.c
	$(CC) -c $(CFLAGS) -o $@ $< $(INC)

obj:
	@mkdir  -p $@

clean:
	-rm $(OBJ) $(TRAGET)
	@rmdir obj

.PHONY: all clean

#.SUFFIXES

vpath %.c  src
vpath %.o  obj
vpath %.h  include
