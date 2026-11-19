# newminhh/compat

The x32 runtime compatibility shared library (`libnewminhh_compat.so`) for the [`newminhh`](https://github.com/newminhh/newminhh) i386-on-x32 compatibility system.

`libnewminhh_compat.so` serves both statically/hybrid-patched binaries ([`newminhh/patcher`](https://github.com/newminhh/patcher)) and the in-process dynamic binary translator ([`newminhh/dbt`](https://github.com/newminhh/dbt)), bridging calling conventions, syscalls, ABI structs, callbacks, dynamic loading, and C/math/pthread APIs to native x32 userspace libraries (`libc.so.6`, `libm.so.6`, `libpthread.so.0`, `libdl.so.2`) while staying page-cache shared across concurrent processes.

---

## Source Files

| File | Role |
| :--- | :--- |
| `Makefile` | Builds `libnewminhh_compat.so` (`-fPIC -O2 -I../common -ldl -lpthread -lm`) and installs into `$(LIBDIR)` (`/usr/local/lib/x86_64-linux-gnux32`) |
| `compat_syms.map` | ELF symbol version script (`NEWMINHH_COMPAT_1.0`) exporting only `newminhh_*` and `compat_*` |
| `compat_asm.S` | x86-64/x32 GAS bridges (`newminhh_call_x32_sig`, `newminhh_call_x32_cdecl`, `newminhh_out_common`, `newminhh_invoke_cdecl_asm`, `newminhh_cb_common`) |
| `compat_callbacks.c` | W^X 16B incoming/outgoing trampoline pools (`512` incoming, `256` outgoing), native cdecl VA range registry, `qsort`/`bsearch`, `pthread_create` `GS` propagation, LIFO `atexit`/`__cxa_atexit`, and `signal`/`sigaction` |
| `compat_syscall.c` | Translates 61 i386 syscall numbers (`int $0x80` / `sysenter`) into x32 `syscall` instructions, including `set_thread_area` (`ARCH_SET_GS`) and `brk` hook isolation |
| `compat_structs.c` | Alignment-safe repacking between i386 and x32 layouts (`stat`/`stat64`/`kstat32`/`stat64_t64`, `timespec32`/`timespec64_t32`, `rlimit32`/`rlimit64_t32`) with `EOVERFLOW` detection |
| `compat_dl.c` | `compat_dlopen` / `compat_dlsym` / `compat_dlsym_cdecl` / `compat_dlclose` / `compat_dlerror` routing canonical system libs, unpatched i386 `.so`s (via DBT backend), and patched/native x32 `.so`s |
| `compat_wrappers.c` | Sorted symbol wrapper registry (`g_sym_table[]`, `bsearch` lookup), data exports (`stdin`/`stdout`/`stderr`/`environ`/`opt*`), `pthread_mutex_t`/`pthread_cond_t` shadow maps, format-aware cdecl variadic printf/vprintf, and POSIX/libc/libm wrappers |

---

## Key Capabilities

- **Bidirectional Calling Convention Marshaling (`compat_asm.S`)**:
  - **Outgoing (i386 cdecl -> x32 SysV)**: Maps 4-byte-strided stack words onto `%rdi..%r9`, `%xmm0..%xmm7`, and 16B-aligned stack spills (`%al = xmm_count`), returning `EDX:EAX` for 64-bit ints and x87 `ST(0)` for floats/doubles.
  - **Incoming (x32 SysV -> Long Mode cdecl)**: Allocates 4-byte stack-strided arguments with `%rsp % 16 == 12` at callee entry (ensuring `[%rsp+4]` is 16B aligned) and routes to either `NM_CB_MODE_NATIVE_CDECL` (`newminhh_invoke_cdecl_asm`) or `NM_CB_MODE_DBT` (`g_guest_invoker`).
- **Pre-Populated W^X Trampoline Pools**: Maps trampoline pages `RW`, writes 16-byte stubs (`movl $slot_id, %r11d; jmp ...`), and flips to `RX` via `mprotect`.
- **Fail-Loudly Policy**: Unsupported syscalls return `-ENOSYS` and log `[newminhh:syscall] ERROR:`; un-wrapped symbols in canonical system libraries return `NULL` and populate `compat_dlerror()` (never returning unmarshaled raw x32 pointers).

---

## Build & Install

Requires [`newminhh/common`](https://github.com/newminhh/common) at `../common` (on a native x32 toolchain with `gcc`):

```bash
make all
make install   # installs libnewminhh_compat.so to $(LIBDIR) and runs ldconfig
make clean
```

## Verification

Tested end-to-end by Part 2 of [`newminhh/newminhh`](https://github.com/newminhh/newminhh) (`tests/part2/test_compat.c` + `test_compat_asm.S`). See [`docs/ARCHITECTURE.md`](https://github.com/newminhh/docs/blob/main/ARCHITECTURE.md) and [`docs/TESTING_AND_VM.md`](https://github.com/newminhh/docs/blob/main/TESTING_AND_VM.md) for details.
