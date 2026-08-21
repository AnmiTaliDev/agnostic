// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 AnmiTaliDev <anmitalidev@nuros.org>
#include "platform/linux/platform.hpp"

namespace agn::platform {

namespace {

constexpr long SYS_read = 0;
constexpr long SYS_write = 1;
constexpr long SYS_open = 2;
constexpr long SYS_close = 3;
constexpr long SYS_mmap = 9;
constexpr long SYS_munmap = 11;
constexpr long SYS_exit_group = 231;
constexpr long O_RDONLY = 0;
constexpr long O_WRONLY_CREAT_TRUNC = 0x1 | 0x40 | 0x200;
constexpr long CREATE_MODE_0644 = 0644;

inline long syscall3(long n, long a1, long a2, long a3) {
    long ret;
    register long r10 asm("r10") = 0;
    register long r8 asm("r8") = 0;
    register long r9 asm("r9") = 0;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return ret;
}

inline long syscall6(long n, long a1, long a2, long a3, long a4, long a5, long a6) {
    long ret;
    register long r10 asm("r10") = a4;
    register long r8 asm("r8") = a5;
    register long r9 asm("r9") = a6;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return ret;
}

} // namespace

void exitProcess(int code) {
    syscall3(SYS_exit_group, code, 0, 0);
    __builtin_unreachable();
}

long readFd(int fd, void* buf, unsigned long count) {
    return syscall3(SYS_read, fd, reinterpret_cast<long>(buf), static_cast<long>(count));
}

long writeFd(int fd, const void* buf, unsigned long count) {
    return syscall3(SYS_write, fd, reinterpret_cast<long>(buf), static_cast<long>(count));
}

void* mapAnonymous(unsigned long size) {
    constexpr long PROT_READ_WRITE = 0x3;
    constexpr long MAP_PRIVATE_ANONYMOUS = 0x22;
    long ret = syscall6(SYS_mmap, 0, static_cast<long>(size), PROT_READ_WRITE,
                        MAP_PRIVATE_ANONYMOUS, -1, 0);
    if (ret < 0 && ret > -4096) return nullptr;
    return reinterpret_cast<void*>(ret);
}

void unmap(void* ptr, unsigned long size) {
    syscall3(SYS_munmap, reinterpret_cast<long>(ptr), static_cast<long>(size), 0);
}

long openRead(const char* path) {
    return syscall3(SYS_open, reinterpret_cast<long>(path), O_RDONLY, 0);
}

long openCreate(const char* path) {
    return syscall3(SYS_open, reinterpret_cast<long>(path), O_WRONLY_CREAT_TRUNC, CREATE_MODE_0644);
}

long closeFd(int fd) {
    return syscall3(SYS_close, fd, 0, 0);
}

extern "C" long agn_argc;
extern "C" const char** agn_argv;

long argCount() { return agn_argc; }

const char* argAt(long index) {
    if (index < 0 || index >= agn_argc) return nullptr;
    return agn_argv[index];
}

} // namespace agn::platform
