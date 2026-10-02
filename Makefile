# se30boot - build with ./make.sh (runs make in the m68k cross-compiler container)
CROSS   ?= m68k-linux-gnu-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
CFLAGS  := -m68030 -msoft-float -Os -std=gnu11 -ffreestanding -fno-builtin \
           -fno-pic -fno-pie -fomit-frame-pointer -ffixed-a5 -Wall -Wextra \
           -Wno-unused-parameter --param=min-pagesize=0
B := build

S2OBJS := $(addprefix $(B)/,start.o main.o lib.o con.o hfs.o inflate.o mmu.o load.o font6x13.o)

all: $(B)/stage1.bin $(B)/se30boot

$(B)/stage1.bin: stage1.S | $(B)
	$(CC) -m68030 -c -o $(B)/stage1.o $<
	$(OBJCOPY) -O binary -j .text $(B)/stage1.o $@
	@ls -l $@

$(B)/se30boot: $(S2OBJS) stage2/link.ld
	$(CC) $(CFLAGS) -nostdlib -static -no-pie -T stage2/link.ld -Wl,--build-id=none \
		-Wl,-Map,$(B)/se30boot.map -o $(B)/se30boot.elf $(S2OBJS) -lgcc
	$(OBJCOPY) -O binary -j .text $(B)/se30boot.elf $@
	@ls -l $@
	@grep -E '__bss_end' $(B)/se30boot.map

$(B)/%.o: stage2/%.c stage2/boot.h | $(B)
	$(CC) $(CFLAGS) -c -o $@ $<

$(B)/inflate.o: CFLAGS += -O2

$(B)/%.o: stage2/%.S | $(B)
	$(CC) $(CFLAGS) -c -o $@ $<

$(B):
	mkdir -p $@

clean:
	rm -rf $(B)

.PHONY: all clean
