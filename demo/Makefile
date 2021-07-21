CFLAGS       = -O
INC          = 
CC           = gcc
LIB          = -lasync_server

BIN     = async_server_demo echo_server_demo
SRC     = $(wildcard *.c)
OBJ     = $(patsubst %.c, %.o, $(SRC))

all: $(BIN)

async_server_demo: async_server_demo.o
	$(CC) -o $@ $^ $(LIB)

echo_server_demo: echo_server_demo.o
	$(CC) -o $@ $^ $(LIB)

$(OBJ): %.o : %.c
	$(CC) -c $(CFLAGS) -o $@ $< $(INC)

clean:
	-rm $(OBJ) $(BIN)

.PHONY: all clean

#.SUFFIXES
