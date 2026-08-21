// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 AnmiTaliDev <anmitalidev@nuros.org>
#if defined(AGN_TARGET_FREEBSD)
#include "platform/freebsd/platform.hpp"
#else
#include "platform/linux/platform.hpp"
#endif

extern "C" long agn_rt_argc() { return agn::platform::argCount(); }

extern "C" const char* agn_rt_argv(long index) { return agn::platform::argAt(index); }

extern "C" long agn_rt_open_read(const char* path) { return agn::platform::openRead(path); }

extern "C" long agn_rt_open_create(const char* path) { return agn::platform::openCreate(path); }

extern "C" long agn_rt_close(long fd) { return agn::platform::closeFd(static_cast<int>(fd)); }

extern "C" long agn_rt_read_fd(long fd, char* buf, long maxlen) {
    return agn::platform::readFd(static_cast<int>(fd), buf, static_cast<unsigned long>(maxlen));
}

extern "C" long agn_rt_write_fd(long fd, const char* buf, long len) {
    return agn::platform::writeFd(static_cast<int>(fd), buf, static_cast<unsigned long>(len));
}

extern "C" [[noreturn]] void agn_rt_exit(long code) { agn::platform::exitProcess(static_cast<int>(code)); }
