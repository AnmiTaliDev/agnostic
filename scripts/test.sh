#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 AnmiTaliDev <anmitalidev@nuros.org>
#
# Regression suite for the compiler: compiles every example through the llvm,
# gcc, and nvm backends and checks the result against known-good behavior.
set -eu

BUILD_DIR="${1:-build}"
AGNOSTIC="$BUILD_DIR/src/cli/agnostic"
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

fail=0

expect_run() {
    backend=$1
    name=$2
    path=$3
    expected_stdout=$4
    out="$WORK_DIR/${name}_${backend}"

    if ! "$AGNOSTIC" "$path" --backend="$backend" --output="$out" >/dev/null 2>&1; then
        echo "FAIL: $name ($backend) did not compile"
        fail=1
        return
    fi

    actual_stdout="$("$out")"
    if [ "$actual_stdout" != "$expected_stdout" ]; then
        echo "FAIL: $name ($backend) stdout mismatch"
        echo "  expected: $expected_stdout"
        echo "  actual:   $actual_stdout"
        fail=1
        return
    fi
    echo "PASS: $name ($backend)"
}

expect_llvm_run() { expect_run llvm "$1" "$2" "$3"; }
expect_gcc_run() { expect_run gcc "$1" "$2" "$3"; }

expect_nvm_compile() {
    name=$1
    path=$2
    should_succeed=$3
    out="$WORK_DIR/${name}_nvm"

    if "$AGNOSTIC" "$path" --backend=nvm --output="$out" >/dev/null 2>&1; then
        rc=0
    else
        rc=1
    fi

    if [ "$should_succeed" = "yes" ] && [ "$rc" -ne 0 ]; then
        echo "FAIL: $name (nvm) expected to compile, but errored"
        fail=1
        return
    fi
    if [ "$should_succeed" = "no" ] && [ "$rc" -eq 0 ]; then
        echo "FAIL: $name (nvm) expected to be rejected, but compiled"
        fail=1
        return
    fi
    echo "PASS: $name (nvm)"
}

expect_freebsd_compile() {
    name=$1
    path=$2
    out="$WORK_DIR/${name}_freebsd"

    if ! "$AGNOSTIC" "$path" --backend=llvm --target-os=freebsd --output="$out" >/dev/null 2>&1; then
        echo "FAIL: $name (freebsd) did not compile"
        fail=1
        return
    fi
    echo "PASS: $name (freebsd, compile-only; llvm+gcc execution verified manually on a FreeBSD VM, not in CI)"
}

expect_llvm_run closures "examples/closures.agn" "$(printf '1\n2\n3\n42')"
expect_llvm_run structs "examples/structs.agn" "$(printf '25\n4\n5\n10')"
expect_llvm_run comptime_platform "examples/comptime_platform.agn" "1"
expect_llvm_run inlineasm "examples/inlineasm.agn" ""
expect_llvm_run hello "examples/hello.agn" "Hello, World!"
expect_llvm_run fizzbuzz "examples/fizzbuzz.agn" \
    "$(printf '1\n2\nFizz\n4\nBuzz\nFizz\n7\n8\nFizz\nBuzz\n11\nFizz\n13\n14\nFizzBuzz\n16\n17\nFizz\n19\nBuzz\nFizz\n22\n23\nFizz\nBuzz\n26\nFizz\n28\n29\nFizzBuzz')"
expect_llvm_run primes "examples/primes.agn" "$(printf '2\n3\n5\n7\n11\n13\n17\n19\n23\n29')"
expect_llvm_run bubble_sort "examples/bubble_sort.agn" "$(printf '1\n2\n3\n4\n5\n7\n8\n9')"
expect_llvm_run strings_demo "examples/strings_demo.agn" "$(printf 'Hello, Agnostic!\n16\n-1\n1\n0\n1\n0')"
expect_llvm_run fibonacci "examples/fibonacci.agn" \
    "$(printf '0\n1\n1\n2\n3\n5\n8\n13\n21\n34\n55\n89\n144\n233\n377')"
expect_llvm_run generics "examples/generics.agn" "$(printf '1\n5\n0\ndivide by zero\n1\n3\n0')"

expect_gcc_run closures "examples/closures.agn" "$(printf '1\n2\n3\n42')"
expect_gcc_run structs "examples/structs.agn" "$(printf '25\n4\n5\n10')"
expect_gcc_run comptime_platform "examples/comptime_platform.agn" "1"
expect_gcc_run inlineasm "examples/inlineasm.agn" ""
expect_gcc_run hello "examples/hello.agn" "Hello, World!"
expect_gcc_run fizzbuzz "examples/fizzbuzz.agn" \
    "$(printf '1\n2\nFizz\n4\nBuzz\nFizz\n7\n8\nFizz\nBuzz\n11\nFizz\n13\n14\nFizzBuzz\n16\n17\nFizz\n19\nBuzz\nFizz\n22\n23\nFizz\nBuzz\n26\nFizz\n28\n29\nFizzBuzz')"
expect_gcc_run primes "examples/primes.agn" "$(printf '2\n3\n5\n7\n11\n13\n17\n19\n23\n29')"
expect_gcc_run bubble_sort "examples/bubble_sort.agn" "$(printf '1\n2\n3\n4\n5\n7\n8\n9')"
expect_gcc_run strings_demo "examples/strings_demo.agn" "$(printf 'Hello, Agnostic!\n16\n-1\n1\n0\n1\n0')"
expect_gcc_run fibonacci "examples/fibonacci.agn" \
    "$(printf '0\n1\n1\n2\n3\n5\n8\n13\n21\n34\n55\n89\n144\n233\n377')"
expect_gcc_run generics "examples/generics.agn" "$(printf '1\n5\n0\ndivide by zero\n1\n3\n0')"

expect_nvm_compile closures "examples/closures.agn" no            # function values/closures unsupported
expect_nvm_compile structs "examples/structs.agn" yes
expect_nvm_compile comptime_platform "examples/comptime_platform.agn" no   # function-valued struct field unsupported
expect_nvm_compile inlineasm "examples/inlineasm.agn" yes
expect_nvm_compile hello "examples/hello.agn" yes
expect_nvm_compile fizzbuzz "examples/fizzbuzz.agn" yes
expect_nvm_compile primes "examples/primes.agn" yes
expect_nvm_compile bubble_sort "examples/bubble_sort.agn" yes
expect_nvm_compile strings_demo "examples/strings_demo.agn" yes
expect_nvm_compile fibonacci "examples/fibonacci.agn" yes
expect_nvm_compile strings_runtime "scripts/testdata/strings_runtime_test.agn" no   # ++ concat unsupported
expect_nvm_compile generics "examples/generics.agn" yes

expect_llvm_run math_stdlib "scripts/testdata/math_test.agn" \
    "$(printf '7\n3\n1024\n9\n6\n12\n120\n1\n0\n10\n55\n1\n0\n55')"
expect_llvm_run strings_runtime "scripts/testdata/strings_runtime_test.agn" \
    "$(printf 'foobar\nhello Agnostic, value=00042')"
expect_llvm_run string_stdlib "scripts/testdata/string_test.agn" \
    "$(printf '5\n0\n-1\n1\nfoobar\n1\n0')"

expect_gcc_run math_stdlib "scripts/testdata/math_test.agn" \
    "$(printf '7\n3\n1024\n9\n6\n12\n120\n1\n0\n10\n55\n1\n0\n55')"
expect_gcc_run strings_runtime "scripts/testdata/strings_runtime_test.agn" \
    "$(printf 'foobar\nhello Agnostic, value=00042')"
expect_gcc_run string_stdlib "scripts/testdata/string_test.agn" \
    "$(printf '5\n0\n-1\n1\nfoobar\n1\n0')"

expect_llvm_run os_string "scripts/testdata/os_string_test.agn" \
    "$(printf 'HELLO\nworld\n6\n-1\n1\n0\n1\n0\n1\n0\n101\nworld\nargcount_ok\n9')"
expect_gcc_run os_string "scripts/testdata/os_string_test.agn" \
    "$(printf 'HELLO\nworld\n6\n-1\n1\n0\n1\n0\n1\n0\n101\nworld\nargcount_ok\n9')"

expect_nvm_compile math_stdlib "scripts/testdata/math_test.agn" yes
expect_nvm_compile string_stdlib "scripts/testdata/string_test.agn" yes
expect_nvm_compile os_string "scripts/testdata/os_string_test.agn" no

expect_freebsd_compile closures "examples/closures.agn"
expect_freebsd_compile structs "examples/structs.agn"
expect_freebsd_compile comptime_platform "examples/comptime_platform.agn"
expect_freebsd_compile inlineasm "examples/inlineasm.agn"
expect_freebsd_compile hello "examples/hello.agn"
expect_freebsd_compile fizzbuzz "examples/fizzbuzz.agn"
expect_freebsd_compile primes "examples/primes.agn"
expect_freebsd_compile bubble_sort "examples/bubble_sort.agn"
expect_freebsd_compile strings_demo "examples/strings_demo.agn"
expect_freebsd_compile fibonacci "examples/fibonacci.agn"
expect_freebsd_compile generics "examples/generics.agn"
expect_freebsd_compile math_stdlib "scripts/testdata/math_test.agn"
expect_freebsd_compile strings_runtime "scripts/testdata/strings_runtime_test.agn"
expect_freebsd_compile string_stdlib "scripts/testdata/string_test.agn"
expect_freebsd_compile os_string "scripts/testdata/os_string_test.agn"

exit $fail
