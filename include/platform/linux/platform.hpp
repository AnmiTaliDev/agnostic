// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 AnmiTaliDev <anmitalidev@nuros.org>
#pragma once

namespace agn::platform {

[[noreturn]] void exitProcess(int code);
long readFd(int fd, void* buf, unsigned long count);
long writeFd(int fd, const void* buf, unsigned long count);
void* mapAnonymous(unsigned long size);
void unmap(void* ptr, unsigned long size);
long openRead(const char* path);
long openCreate(const char* path);
long closeFd(int fd);
long argCount();
const char* argAt(long index);

} // namespace agn::platform
