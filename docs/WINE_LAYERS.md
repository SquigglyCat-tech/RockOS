# Wine on RockOS — layered port plan

Wine cannot land in one step on a freestanding, no-libc kernel. This
roadmap adds it layer by layer, each layer independently buildable and
testable, with the whole build staying green in CI after every merge.

| # | Layer | What it gives Wine | Status |
|---|-------|--------------------|--------|
| 0 | User memory + syscall surface | `mmap`/`brk`/`munmap`, read/write, PID/sleep syscalls plus a userspace assembly test program exercising them | **in progress (this PR)** |
| 1 | POSIX-shaped syscall API | mmap with hints, `exec`-able image loader (ELF64 first, flat/PE next), file descriptors via RockFS, fork/clone | planned |
| 2 | libc for userspace | link a freestanding-friendly libc (musl, built with `--target=rockos`) into ring-3 executables | planned |
| 3 | Process model hardening | real per-process FPU/SSE state, argument passing on stack, environment block, PID table, dynamic loader (`ld-so` analog) | planned |
| 4 | Winelib toolchain | build Wine itself against layer 2/3 (Wine64 + Wow64 32-bit sub-arch = runs Win32 binaries) | planned |
| 5 | Windowing bridge | project Wine's `user32`/`gdi32` output onto RockOS desktop shell | planned |
| 6 | Drivers + perf | audio mapping, GPU blit path beyond the framebuffer, faster PMM beyond the current 1 GiB map limit | planned |

## Layer 0 details (this change)

- Syscall numbers (int 0x80): 0 exit, 1 putchar, 5 mmap, 6 munmap.
  Argument in RDI (64-bit for mmap/munmap, ASCII byte for putchar).
- `mmap` hands out fresh 4 KiB pages from the physical allocator and
  maps them USER|RW into the current address space at a kernel-chosen
  base above the test program's code/stack region; returns the virtual
  address or `(uint64_t)-1` on failure.
- `munmap` unmaps+releases pages previously handed out by `mmap` only
  (mapping table tracked in-kernel), refusing foreign addresses.
- A new user-mode test program (`ring3_memory_program`) mmaps a page,
  writes `0x5A`, reads it back, prints `M` on success / `X` on
  mismatch, and exits — run from the boot sequence next to the
  existing ring-3 test.
