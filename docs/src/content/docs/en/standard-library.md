---
title: Standard Library
description: stdio, math, string, os, and novaria.
---

Every function in these modules is a compiler intrinsic: the body written in the `.agn` source exists only so the file type-checks and so a human reading it can see the signature. The actual behavior is generated directly by the compiler backend, not by running that body. `ReadInt`, `ReadChar`, `ReadLine`, template string interpolation, and `++` are the exceptions called out below where the source body's stated behavior does not hold under `--backend=nvm`.

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
| `indexOf` | `(s string, sub string) int` | index of the first occurrence of `sub`, or -1 |
| `contains` | `(s string, sub string) int` | 1 or 0 |
| `startsWith` | `(s string, prefix string) int` | 1 or 0 |
| `endsWith` | `(s string, suffix string) int` | 1 or 0 |
| `charAt` | `(s string, index int) int` | byte code at `index`, or -1 if out of bounds |
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
| `OpenRead` | `(path string) int` | file descriptor, or negative on error |
| `OpenCreate` | `(path string) int` | create/truncate for writing, file descriptor or negative on error |
| `Close` | `(fd int) int` | |
| `ReadFd` | `(fd int, buffer int, maxlen int) int` | reads into a heap buffer address, like `stdio.ReadLine`; returns bytes read or negative on error |
| `WriteFd` | `(fd int, data string) int` | bytes written, or negative on error |
| `Exit` | `(code int)` | terminates the process immediately, bypassing any remaining code in the caller |

`ReadFd`'s (and `stdio.ReadLine`'s) `buffer` parameter is typed `int` but accepts the pointer produced by `&arr` directly — a pointer value assigned to an `int` parameter decays to its address.

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
