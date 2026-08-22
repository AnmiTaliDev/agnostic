---
title: Structs
description: Fields, methods, and struct literals.
---

## Declaration

```agn
struct Vector2 {
    x i64,
    y i64
}
```

Fields are comma-separated `name type` pairs. There is no inheritance and no vtables: a struct cannot extend another struct, and methods are resolved statically at compile time, not through a dispatch table.

## Struct literals

```agn
var a Vector2 = Vector2{x: 3, y: 4}
```

Every field must be named in the literal (`field: value`); there is no positional form.

## Methods

A method is a function with a receiver in parentheses before the name:

```agn
func (v Vector2) Length2() i64 {
    return v.x * v.x + v.y * v.y
}

func (v Vector2) Add(other Vector2) Vector2 {
    return Vector2{x: v.x + other.x, y: v.y + other.y}
}
```

Call it with `.`:

```agn
var a Vector2 = Vector2{x: 3, y: 4}
stdio.Println(a.Length2())   // 25
```

The receiver is passed by reference (as a hidden pointer to the caller's variable), not copied. Assigning to a receiver field inside a method mutates the caller's own struct:

```agn
func (v Vector2) SetX(newX i64) {
    v.x = newX
}

var a Vector2 = Vector2{x: 3, y: 4}
a.SetX(99)
stdio.Println(a.x)   // 99
```

## Function-typed fields

A struct field can hold a function value:

```agn
struct Platform {
    write func(i64, i64, i64) -> i64
}

var platform Platform
platform.write = linuxWrite
stdio.Println(platform.write(1, 0, 4))
```

This only works under `--backend=llvm`, for the same reason plain closures do not work under `--backend=nvm`: see [Functions and Closures](/en/functions/).

## Field access and assignment

```agn
var a Vector2 = Vector2{x: 3, y: 4}
a.x = 10
stdio.Println(a.x)
```

## Generic structs

Structs can take type parameters, resolved at compile time by monomorphization: each distinct
combination of type arguments used in the program (`Result<int,string>`, `Result<int,bool>`, ...)
generates its own concrete struct behind the scenes. There are no generic functions and no pattern
matching — just generic struct declarations and instantiations.

```agn
struct Pair<A, B> {
    first A
    second B
}

var p Pair<int,string> = Pair<int,string>{first: 1, second: "one"}
stdio.Println(p.first)
stdio.PrintlnStr(p.second)
```

The compiler predefines two generic structs, always in scope without an import: `Result<T,E>`
(`ok bool`, `value T`, `err E`) and `Option<T>` (`some bool`, `value T`) — used for functions where a
plain `int` sentinel return isn't precise enough.

```agn
func safeDivide(a int, b int) Result<int,string> {
    if b == 0 {
        return Result<int,string>{ok: false, value: 0, err: "divide by zero"}
    }
    return Result<int,string>{ok: true, value: a / b, err: ""}
}

var r Result<int,string> = safeDivide(10, 0)
if !r.ok {
    stdio.PrintlnStr(r.err)
}
```

As with any struct, assign the result to a local variable before accessing its fields — `.field`
access on a call result or a chained `.field.field` isn't supported (see [Field access and
assignment](#field-access-and-assignment) above).
