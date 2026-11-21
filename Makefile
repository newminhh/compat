ifeq ($(origin CC),default)
  CC := clang
endif
CSTD ?= gnu23

IS_CLANG := $(shell $(CC) --version 2>/dev/null | grep -qi clang && echo 1 || echo 0)

CFLAGS_COMMON := -fPIC -O2 -std=$(CSTD) -Wall -Wextra -Wpedantic \
	-Wstrict-prototypes -Wold-style-definition -Wshadow -Wundef -Wformat=2 \
	-fstrict-flex-arrays=3 -Werror -I../common

ifeq ($(IS_CLANG),1)
CFLAGS := $(CFLAGS_COMMON) -Wnullability-completeness -Wno-nullability-extension -Wno-gcc-compat -Wno-deprecated-octal-literals
else
CFLAGS := $(CFLAGS_COMMON)
endif

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

.PHONY: all clean install analyze

all: $(TARGET)

%.o: %.c ../common/newminhh_cstd.h ../common/newminhh_abi.h ../common/newminhh_log.h
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S ../common/newminhh_cstd.h ../common/newminhh_abi.h
	$(CC) $(ASFLAGS) -c $< -o $@

$(TARGET): $(OBJS) compat_syms.map
	$(CC) $(LDFLAGS) $(OBJS) $(LIBS) -o $@

ifeq ($(IS_CLANG),1)
analyze:
	@for f in $(C_SRCS); do \
		echo "Analyzing $$f (clang)..."; \
		$(CC) $(CFLAGS) --analyze $$f -o /dev/null || exit 1; \
	done
else
analyze:
	@for f in $(C_SRCS); do \
		echo "Analyzing $$f (gcc)..."; \
		$(CC) $(CFLAGS) -fanalyzer -fsyntax-only $$f || exit 1; \
	done
endif

install: $(TARGET)
	install -d $(DESTDIR)$(LIBDIR)
	install -m 0755 $(TARGET) $(DESTDIR)$(LIBDIR)/$(TARGET)
	ldconfig || true

clean:
	rm -f $(OBJS) $(TARGET)
