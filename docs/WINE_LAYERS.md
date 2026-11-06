# Wine on RockOS — layered port plan

Wine cannot land in one step on a freestanding, no-libc kernel. This
roadmap adds it layer by layer, each layer independently buildable and
testable, with the whole build staying green in CI after every merge.

| # | Layer | What it gives Wine | Status |
|---|-------|--------------------|--------|
| 0 | User memory + syscall surface | `mmap`/`brk`/`munmap`, read/write, PID/sleep syscalls plus a userspace assembly test program exercising them | **in progress (this PR)** |
| 1 | Executable loader + launch | **PE32/PE32+ (`.exe`) parser + mapping into a ring-3 address space, entry launch, POSIX-shaped syscalls (read/write fds via RockFS, hints in mmap, fork/clone)** — start with a Win32-API-free (pure) PE, then imports via kernel32/Wine | **parser landed (host-verified), mapping+launch next** |
| 2 | libc for userspace | link a freestanding-friendly libc (musl, built with `--target=rockos`) into ring-3 executables | planned |
| 3 | Process model hardening | real per-process FPU/SSE state, argument passing on stack, environment block, PID table, dynamic loader (`ld-so` analog) | planned |
| 4 | Winelib toolchain | build Wine itself against layer 2/3 (Wine64 + Wow64 32-bit sub-arch = runs Win32 binaries) | planned |
| 5 | Windowing bridge | project Wine's `user32`/`gdi32` output onto RockOS desktop shell | planned |
| 6 | Drivers + perf | audio mapping, GPU blit path beyond the framebuffer, faster PMM beyond the current 1 GiB map limit | planned |

## Layer 1 landing order (per "we need this to run exes")

- **1a (this change):** `include/pe.h` / `src/kernel/pe.c` parse DOS+COFF
- OptionalHeader, section table, characteristics (`.text` exec / `.data`
- rw), and entry-point RVA for both PE32 (i386) and PE32+ (AMD64) images.
- Host-verified with `tests/pe_host_test.c`: a synthetic PE32, a
- synthetic PE32+ with two sections, plus rejection cases (bad magic,
- bad machine, too many sections, missing optional header, section
- bytes out of range). All PASS on the host gcc build.
- **1b next:** kernel mapping (`paging_map_user_page` per section,
- headers page, entry page RW-X flags via current ring-3 plumbing) and
- launch (`runexe` shell command / `pe_test` boot task). Loading the
- exe file comes from RockFS via `filesystem_read`.
- **1c after:** import table resolution into a Win32 API surface
- (kernel32 first: `VirtualAlloc`, `ExitProcess`, `WriteFile`,
- `GetStdHandle`/console) so regular Win32 exes run. Only import-free
- (hardcoded, no CRT) small exes run in step 1a/1b.

## Layer 0 details

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
