CROSS_COMPILE ?=
CC      := $(CROSS_COMPILE)gcc
CFLAGS  ?= -Wall -Wextra -Werror -g
LDFLAGS ?=
TARGET  := aesdsocket
SRC     := aesdsocket.c
 
.PHONY: all default clean
 
all: $(TARGET)
 
default: $(TARGET)
 
$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(INCLUDES) $^ -o $@ $(LDFLAGS)
 
clean:
	rm -f $(TARGET) *.o
 

