---
title: Syntax
description: Packages, functions, variables, comments, operators, and string literals.
---

## Source files

A source file is one `package` declaration followed by top-level `import` statements and function and struct declarations. Statements are separated by newlines, not semicolons. Indentation is not significant.

```agn
package main

import "stdio"

func main() {
    stdio.PrintlnStr("Hello, World!")
}
```

The entry point is a function named `main` in `package main`.

## Comments

Two forms of line comment, both extending to the end of the line:

```agn
// a comment
# also a comment
```

There are no block comments.

## Imports

```agn
import "stdio"
import "math"
```

`use` is accepted as an alias for `import`. See [Modules](/en/modules/) for how import paths resolve to files.

## Functions

```agn
func add(a int, b int) int {
    return a + b
}
```

`fn` is accepted as an alias for `func`. Parameters are `name type`, comma-separated. The return type follows the parameter list with no arrow; omit it for a function that returns nothing.

Functions declared in an imported module need the `pub` modifier to be visible to the importer:

```agn
pub fn Max(a int, b int) int {
    if a > b {
        return a
    }
    return b
}
```

A module function without `pub` is not type-checked or compiled at all when the module is imported. `pub` has no effect on functions in `package main` itself.

## Variables

```agn
var x int = 5
var y = 5        // type inferred from the initializer
var z int        // zero value
```

`let` is accepted as an alias for `var`. A colon between the name and the type (`var x: int = 5`) is accepted and optional.

Assignment to an existing variable, an array element, a struct field, or through a pointer uses `=` with no `var`:

```agn
x = 6
arr[0] = 6
point.x = 6
*ptr = 6
```

## Operators

Arithmetic: `+`, `-`, `*`, `/`, `%`. Comparison: `==`, `!=`, `<`, `<=`, `>`, `>=`. Logical: `&&`, `||`, `!`. Bitwise: `&`, `|`, `^`, `<<`, `>>`. String concatenation: `++` (works under `--backend=llvm`; `--backend=nvm` rejects it at compile time, use `string.concat` instead, see [Standard Library](/en/standard-library/)).

```agn
var greeting string = "foo" ++ "bar"
```

There is no operator overloading.

## Literals

Integer literals are decimal only: `0`, `42`, `1000000`. There is no hexadecimal, octal, or binary literal syntax. A literal with a decimal point (`3.14`) is an `f64` literal; see [Types](/en/types/). `true` and `false` are `bool` literals.

String literals are double-quoted with `\n`, `\t`, `\r`, `\\`, and `\"` escapes:

```agn
var s string = "line one\nline two"
```

A string can be indexed to get the byte at that position, as an integer:

```agn
var c int = "hello"[0]   // 104, the byte value of 'h'
```

## String interpolation

A string literal containing `$(expr)` is a template string: the expression is evaluated and its text form is spliced in at that point.

```agn
var name string = "Agnostic"
var n int = 42
stdio.PrintlnStr("hello $(name), value=$(n)")
```

An interpolated expression can carry a format spec after a colon: an optional leading `0` for zero-padding, an optional width, and one of `d` (decimal), `x` (lowercase hex), `X` (uppercase hex), or `s` (string).

```agn
stdio.PrintlnStr("value=$(n:05d)")   // value=00042
```

Template strings are evaluated at runtime and are only available under `--backend=llvm`; `--backend=nvm` rejects them at compile time.
