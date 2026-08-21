---
title: Standard Library
description: result, stdio, math, string, os, and novaria.
---

Every function in these modules is a compiler intrinsic: the body written in the `.agn` source exists only so the file type-checks and so a human reading it can see the signature. The actual behavior is generated directly by the compiler backend, not by running that body. `ReadInt`, `ReadChar`, `ReadLine`, template string interpolation, and `++` are the exceptions called out below where the source body's stated behavior does not hold under `--backend=nvm`.

## result

`stdlib/result.agn` declares `Result<T,E>` (`ok int`, `value T`, `err E`) and `Option<T>` (`some int`, `value T`). Unlike every other module here, these two generic structs are always in scope — no `import "result"` needed — and their fields are real data, not a compiler-generated body; the compiler parses this file directly and merges its declarations into every compiled program. See [Generic structs](/en/structs/#generic-structs) for how instantiation (`Result<int,string>`) and monomorphization work.

## stdio

| Function | Signature | Notes |
|---|---|---|
| `Print` | `(value int)` | writes a decimal integer, no newline |
| `Println` | `(value int)` | writes a decimal integer and a newline |
| `PrintStr` | `(text string)` | writes a string, no newline |
| `PrintlnStr` | `(text string)` | writes a string and a newline |
| `PrintChar` | `(ch int)` | writes one byte |
| `ReadInt` | `() int` | reads a decimal integer from stdin |
| `ReadChar` | `() int` | reads one byte from stdin |
| `ReadLine` | `(buffer int, maxlen int) int` | reads a line into a heap buffer, returns the byte count |
| `Flush` | `()` | no-op; output is unbuffered |

`Print` and `Println` only accept `int`; passing a `string` is a type error, use `PrintStr`/`PrintlnStr`.

`ReadInt`, `ReadChar`, and `ReadLine` are compile errors under `--backend=nvm`. The Novaria kernel's stdin file descriptor has no working read handler (`/dev/stdin` has a null read function, `/dev/tty` returns immediately without filling the buffer), so there is no correct behavior to generate; the compiler refuses to compile the call instead of producing a program that hangs or reads garbage.

## math

All functions take and return `int`.

| Function | Signature | Notes |
|---|---|---|
| `Max` | `(a int, b int) int` | |
| `Min` | `(a int, b int) int` | |
| `Pow` | `(base int, exp int) int` | |
| `Sqrt` | `(n int) int` | integer square root, Newton's method, positive input only |
| `GCD` | `(a int, b int) int` | Euclidean algorithm, positive input only |
| `LCM` | `(a int, b int) int` | positive input only |
| `Fact` | `(n int) int` | factorial |
| `IsEven` | `(n int) int` | 1 or 0 |
| `IsOdd` | `(n int) int` | 1 or 0 |
| `Sign` | `(x int) int` | 1 if `x > 0`, otherwise 0; negative input is not distinguished from zero |
| `Clamp` | `(value int, min int, max int) int` | |
| `SumRange` | `(n int) int` | sum of `1..n` |
| `IsPrime` | `(n int) int` | trial division, 1 or 0 |
| `Fib` | `(n int) int` | n-th Fibonacci number, iterative |

## string

| Function | Signature | Notes |
|---|---|---|
| `len` | `(s string) int` | byte length |
| `compare` | `(s1 string, s2 string) int` | 0 if equal, -1 if `s1 < s2`, 1 if `s1 > s2` |
| `concat` | `(s1 string, s2 string) string` | |
| `is_empty` | `(s string) int` | 1 or 0 |
| `indexOf` | `(s string, sub string) Option<int>` | index of the first occurrence of `sub`; `some=0` if not found |
| `contains` | `(s string, sub string) int` | 1 or 0 |
| `startsWith` | `(s string, prefix string) int` | 1 or 0 |
| `endsWith` | `(s string, suffix string) int` | 1 or 0 |
| `charAt` | `(s string, index int) Option<int>` | byte code at `index`; `some=0` if out of bounds |
| `substr` | `(s string, start int, len int) string` | clamped to the string's bounds |
| `toUpper` | `(s string) string` | ASCII only |
| `toLower` | `(s string) string` | ASCII only |

`++` and `$(...)` template string interpolation (see [Syntax](/en/syntax/)) cover the same ground as `concat` for simple cases; `string.concat` is the only string-building option available under `--backend=nvm`, where `++` and template strings are compile errors. `indexOf`, `contains`, `startsWith`, `endsWith`, `charAt`, `substr`, `toUpper`, and `toLower` are compile errors under `--backend=nvm`.

## os

File descriptors and command-line arguments. Compile errors under `--backend=nvm`; use `novaria` there instead.

| Function | Signature | Notes |
|---|---|---|
| `ArgCount` | `() int` | number of command-line arguments, including argv[0] (the program path) |
| `Arg` | `(index int) string` | argument at `index` |
| `OpenRead` | `(path string) Option<int>` | value is the file descriptor; `some=0` on error |
| `OpenCreate` | `(path string) Option<int>` | create/truncate for writing; value is the file descriptor, `some=0` on error |
| `Close` | `(fd int) int` | |
| `ReadFd` | `(fd int, buffer int, maxlen int) Option<int>` | reads into a heap buffer address, like `stdio.ReadLine`; value is bytes read, `some=0` on error |
| `WriteFd` | `(fd int, data string) Option<int>` | value is bytes written, `some=0` on error |
| `Exit` | `(code int)` | terminates the process immediately, bypassing any remaining code in the caller |

`ReadFd`'s (and `stdio.ReadLine`'s) `buffer` parameter is typed `int` but accepts the pointer produced by `&arr` directly — a pointer value assigned to an `int` parameter decays to its address.

`OpenRead`, `OpenCreate`, `ReadFd`, and `WriteFd` return `Option<int>` (see [Generic structs](/en/structs/#generic-structs)) instead of a raw sentinel: `some` is 1 on success and 0 on failure, and `value` holds the file descriptor or byte count only when `some` is 1 — check `some` before trusting `value`.

## novaria

| Function | Signature | Notes |
|---|---|---|
| `Exit` | `(code int)` | |
| `Open` | `(filename string) int` | read-only; there is no syscall to create or write a file |
| `Read` | `(fd int, bufferOffset int) int` | |
| `Write` | `(fd int, bufferOffset int) int` | |
| `Remove` | `(filename string)` | |
| `Exec` | `(filename string)` | opens then spawns; the kernel does not report the child's PID back |
| `MemAlloc` | `(size int) int` | returns a heap offset |

This module wraps syscalls specific to the Novaria kernel. Its intrinsic behavior is only generated under `--backend=nvm`; under `--backend=llvm` these functions compile to their literal, non-functional source body (`Open` always returns `-1` and so on), because there is no Novaria kernel underneath a native `llvm`-backend executable.
