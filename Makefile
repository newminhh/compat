CC := gcc
CFLAGS := -fPIC -O2 -Wall -Wextra -Werror=implicit-function-declaration -I../common
ASFLAGS := -fPIC -I../common
LDFLAGS := -shared -Wl,--version-script=compat_syms.map -Wl,-soname,libnewminhh_compat.so -Wl,-z,noexecstack -Wl,-z,relro -Wl,-z,now
LIBS := -ldl -lpthread -lm

PREFIX ?= /usr/local
LIBDIR ?= $(PREFIX)/lib/x86_64-linux-gnux32

TARGET := libnewminhh_compat.so

C_SRCS := \
	compat_syscall.c \
	compat_structs.c \
	compat_callbacks.c \
	compat_dl.c \
	compat_wrappers.c

S_SRCS := compat_asm.S

OBJS := $(C_SRCS:.c=.o) $(S_SRCS:.S=.o)

.PHONY: all clean install

all: $(TARGET)

%.o: %.c ../common/newminhh_abi.h ../common/newminhh_log.h
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S ../common/newminhh_abi.h
	$(CC) $(ASFLAGS) -c $< -o $@

$(TARGET): $(OBJS) compat_syms.map
	$(CC) $(LDFLAGS) $(OBJS) $(LIBS) -o $@

install: $(TARGET)
	install -d $(DESTDIR)$(LIBDIR)
	install -m 0755 $(TARGET) $(DESTDIR)$(LIBDIR)/$(TARGET)
	ldconfig || true

clean:
	rm -f $(OBJS) $(TARGET)
