CROSS   ?= riscv64-unknown-elf-
CC       = $(CROSS)gcc
CFLAGS   = -static -mabi=lp64d -O1 -g

TARGET   = lrsc_stress
SRC      = lrsc_stress.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)
