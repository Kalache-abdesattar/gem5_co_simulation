CROSS   ?= riscv64-unknown-elf-
CC       = $(CROSS)gcc
# -nostdlib: no libc/crt; _start in lrsc_stress.S provides the entry point
CFLAGS   = -static -march=rv64imafd -mabi=lp64d -nostdlib -g

TARGET   = lrsc_stress
SRC      = lrsc_stress.S

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)
