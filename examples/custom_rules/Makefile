CC      = qcc
TARGET  ?= -Vgcc_ntox86_64
CFLAGS  ?= $(TARGET) -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -I../../include -I../../guest/include
LDFLAGS ?= -lm

all: example_custom_rules

example_custom_rules: main.c ../../libsafepulse.a
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	rm -f example_custom_rules *.o
