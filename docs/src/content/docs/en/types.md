---
title: Types
description: The Agnostic type system.
---

## Scalars

| Type | Meaning |
|---|---|
| `i64` | 64-bit signed integer |
| `i32` | 32-bit signed integer |
| `i8` | 8-bit signed integer |
| `u64` | 64-bit unsigned integer |
| `u32` | 32-bit unsigned integer |
| `u8` | 8-bit unsigned integer |
| `f64` | 64-bit floating point |
| `bool` | boolean |
| `string` | string |
| `int` | alias for `i64` |
| `float` | alias for `f64` |

A literal with a `.` (`3.14`) is `f64`; a literal without one (`3`) is `i64`. Mixing `f64` and an
integer in arithmetic (`3.14 + 2`) implicitly converts the integer operand to `f64` and the
expression's type is `f64`; `%` and the bitwise/shift operators do not accept `f64` operands.
`f64` is only supported under `--backend=llvm` and `--backend=gcc` — `--backend=nvm` rejects any
use of `f64` at compile time, since its bytecode stack machine is 32-bit-integer-only throughout
and there is no in-repo interpreter to verify a float encoding against.

There is no `char` type; a byte read from a string index or `stdio.ReadChar` is an `int`.

## Type inference

`var x = expr` infers the type of `x` from `expr`. `var x int = expr` declares the type explicitly and checks `expr` against it. A declaration with no initializer (`var x int`) gets the type's zero value.

## Pointers

A pointer type only arises from taking the address of something with `&`; there is no syntax to write a pointer type directly (no `*T` in a type position). Because of this, a pointer-typed variable must use inferred declaration:

```agn
var x int = 5
var p = &x   // p has type "pointer to i64"
*p = 99      // x is now 99
```

Pointers are only supported under `--backend=llvm`. `--backend=nvm` rejects `&` at compile time: ordinary local variables in the Novaria Virtual Machine's bytecode are frame-relative stack slots, not addressable memory.

## Arrays

Fixed size, declared with the size and element type before the variable is usable:

```agn
var arr [8]int
arr[0] = 5
var first int = arr[0]
```

Array size is a compile-time integer literal. There is no array literal syntax and no slicing; every element is set individually or in a loop.

## Structs

See [Structs](/en/structs/).

## Function types

Written `func(T1, T2) -> R`, or `func() -> R` with no parameters, or `func(T1)` with an inferred `void` return. See [Functions and Closures](/en/functions/).

## Type checking rules

Types are checked, not coerced across boundaries that would lose information silently. Assignment between different integer widths is permitted where the typechecker's `canAssignTo` rule allows it (unsigned and signed integer kinds interconvert freely at the type-check level; the emitted code truncates or extends as needed for the target width). There is no implicit conversion between `string` and any numeric type.

## Generics and pattern matching

Generic *structs*, resolved at compile time by monomorphization, exist — see [Generic
structs](/en/structs/#generic-structs). There are no generic *functions*, and there is no
`match`/`switch` expression; branching is `if`/`else` only (see [Control Flow](/en/control-flow/)).
