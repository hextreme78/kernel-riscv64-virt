ARCH = riscv64
PLATFORM = virt
TOOLCHAIN_PREFIX = riscv64-flux
TARGET = kernel-$(ARCH)-$(PLATFORM)

QEMU_SMP = 1
QEMU_MEM = 256M

CC = $(TOOLCHAIN_PREFIX)-gcc
LD = $(TOOLCHAIN_PREFIX)-ld
QEMU = qemu-system-$(ARCH)

SOURCELIST = \
	$(wildcard \
		kernel/*.[c|S] \
	)
OBJECTLIST = \
	$(filter %.o, $(patsubst \
		%.c, \
		%.o, \
		$(SOURCELIST) \
	)) \
	$(filter %.o, $(patsubst \
		%.S, \
		%.o, \
		$(SOURCELIST) \
	))

CFLAGS = \
	$(shell sed -e 's/^/-D/' config) \
	-Iinclude \
	-c \
	-Wall \
	-std=gnu99 \
	-mcmodel=medany \
	-ffreestanding \
	-fno-pie \
	-Og \
	-g

LDFLAGS = \
	-Tkernel/kernel.ld \
	-nostdlib \
	-static

QEMUFLAGS = \
	-machine $(PLATFORM) \
	-bios none \
	-kernel $(TARGET) \
	-serial stdio \
	-m $(QEMU_MEM) \
	-smp $(QEMU_SMP) \
	-global virtio-mmio.force-legacy=false \
	-drive file=root.fs,if=none,format=raw,id=hd \
	-device virtio-blk-device,drive=hd

QEMUFLAGS_GDB = $(QEMUFLAGS) -s -S

all: $(TARGET)

$(TARGET): $(OBJECTLIST)
	$(LD) $(OBJECTLIST) -o $(TARGET) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) $< -o $@

%.o: %.S
	$(CC) $(CFLAGS) $< -o $@

qemu:
	$(QEMU) $(QEMUFLAGS)

qemu-gdb:
	$(QEMU) $(QEMUFLAGS_GDB)

clean:
	rm $(OBJECTLIST) $(TARGET)

