---
title: Toolchain
description: CLI flags, backends, memory modes, and packaging.
---

## CLI flags

```
agnostic <source.agn> [options]
  --backend=llvm|nvm|gcc   select codegen backend (default: llvm)
  --mem=arc|manual|orc     select memory management mode (default: arc)
  --target-os=linux|freebsd|windows|hurd  (default: linux, only linux implemented)
  --output=<path>          output executable path
  --version                print version and exit
  --help                   print usage and exit
```

`--llvm`, `--nvm`, and `--gcc` are accepted as shorthand for the matching `--backend=` value.

## Backends

### llvm (default)

Generates native machine code through the LLVM C++ API (`IRBuilder`, `Module`, `TargetMachine`), then links it into a static executable with `cc -nostdlib -static -no-pie -e _start`. The result does not link libc; the standard library and runtime call the Linux kernel through raw syscalls (`src/platform/linux`).

### nvm

Generates bytecode for the Novaria Virtual Machine and writes it to `<output>.bin`. Running it needs a Novaria kernel or a separate bytecode interpreter; this repository does not ship one. Not supported under this backend: closures and function values, `&` address-of, template string interpolation, the `++` string operator, the `f64` type, and `stdio.ReadInt`/`ReadChar`/`ReadLine` (the Novaria kernel's stdin has no working read path). Each of these is a compile-time error naming the specific construct, not a silent miscompile.

### gcc

Generates native machine code through libgccjit, GCC's embeddable code generation library, and links it the same way the `llvm` backend does. It has the same feature set as `llvm` (closures, structs, pointers, `comptime`, all three `--mem=` modes) and shares the same runtime static libraries and libc-free linking. Building the compiler with this backend needs `libgccjit.h` and `libgccjit.so` on the system (Arch Linux: package `libgccjit`; Fedora: `libgccjit-devel`; Debian/Ubuntu: `libgccjit-dev`).

## Memory modes

`--mem=` only affects the `llvm` backend; `nvm` bytecode has no heap allocation runtime to select a strategy for.

- **arc** (default): reference counting. `agn_rt_retain`/`agn_rt_release` run around bindings that alias an existing value; a fresh construction (a literal, a call result, a struct literal) is not retained again because it already owns its one reference.
- **manual**: `agn_rt_alloc` only. Nothing is freed automatically.
- **orc**: region-based. Every function call opens a region on entry and frees everything allocated in it, in bulk, on every return path, including `main`. This is cheaper than `arc` but has a real limitation: a pointer allocated inside a function and returned to (or stored somewhere reachable from) the caller is unsafe once that function's region has been torn down. There is no escape analysis to catch this; it is the caller's responsibility.

## target-os

`linux` and `freebsd` have real platform implementations; `windows` and `hurd` are unimplemented stubs and fail with "only --target-os=linux and --target-os=freebsd have a real platform/runtime implementation". The `freebsd` target links raw amd64 syscalls directly (no libc) and brands the resulting static ELF with `EI_OSABI=ELFOSABI_FREEBSD`, same as `brandelf -t FreeBSD`. Verified by running compiled binaries from both the `llvm` and `gcc` backends (including closures and structs, which exercise the heap allocator) on a real FreeBSD 15.1 VM.

## Diagnostics

Type errors report a real `file:line:column`, a colored pointer into the source line, and, where the compiler finds a close match, a suggestion:

```
error: variable 'countr' not declared (did you mean 'counter'?) (in main)
  --> hello.agn:6:5
```

Suggestions are Levenshtein-distance based and only appear when a close-enough candidate exists: undeclared variables (checked against locals and top-level functions in scope), unknown struct fields, and unknown struct names. For `object.member(...)`, the compiler first checks whether `object` itself is a known module, struct-typed variable, or import (`stdi.Println` suggests `stdio`); if `object` resolves but `member` does not, it suggests among that struct's or module's own members, including struct fields holding a function value alongside actual methods (`p.writ(...)` suggests the field `write`).

## Building the compiler from source

```sh
cmake -S . -B build
cmake --build build -j$(nproc)
```

Requires LLVM development files on the CMake search path and a C++20 compiler. See [Getting Started](/en/getting-started/).

## Installing from a package

CI builds `.deb`, `.rpm`, an Arch package, and an Alpine `.apk` on every tag push, and attaches them to the GitHub release. All four install the same layout: the `agnostic` binary under `bin/`, the runtime static libraries under `lib/agnostic/`, and the standard library `.agn` files under `share/agnostic/stdlib/`. The compiler looks for its standard library and runtime libraries relative to its own path first, then falls back to the layout used when running straight out of the build directory, so an installed package and a locally built binary both work without extra configuration.
