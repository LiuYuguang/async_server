CFLAGS := -O -DDEBUG -g
INC    := -I./include
CC     := gcc

BIN    := main echo_server
SRC    := $(wildcard src/*.c)
OBJS   := $(patsubst src/%.c, obj/%.o, $(SRC))

all: $(BIN)

main: main.o async_server.o http_parser.o iso8583_parser.o local_protocol.o rbtree.o
	$(CC) -o $@ $^

echo_server: echo_server.o local_protocol.o
	$(CC) -o $@ $^

$(OBJS): obj/%.o : src/%.c
	$(CC) -c $(CFLAGS) -o $@ $< $(INC)

clean:
	-rm $(OBJS) $(BIN)

.PHONY: all clean 

#.SUFFIXES

vpath %.c  src
vpath %.o  obj
vpath %.h  include
