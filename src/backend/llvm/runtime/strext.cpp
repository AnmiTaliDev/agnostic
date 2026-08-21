// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 AnmiTaliDev <anmitalidev@nuros.org>

extern "C" unsigned long agn_rt_strlen(const char* s);
extern "C" void* agn_rt_alloc(unsigned long size);

namespace {

char toUpperChar(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }
char toLowerChar(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

} // namespace

extern "C" long agn_rt_index_of(const char* s, const char* sub) {
    unsigned long slen = agn_rt_strlen(s);
    unsigned long sublen = agn_rt_strlen(sub);
    if (sublen == 0) return 0;
    if (sublen > slen) return -1;
    for (unsigned long i = 0; i + sublen <= slen; i++) {
        unsigned long j = 0;
        while (j < sublen && s[i + j] == sub[j]) j++;
        if (j == sublen) return static_cast<long>(i);
    }
    return -1;
}

extern "C" long agn_rt_contains(const char* s, const char* sub) { return agn_rt_index_of(s, sub) >= 0 ? 1 : 0; }

extern "C" long agn_rt_starts_with(const char* s, const char* prefix) {
    unsigned long i = 0;
    while (prefix[i] != '\0') {
        if (s[i] != prefix[i]) return 0;
        i++;
    }
    return 1;
}

extern "C" long agn_rt_ends_with(const char* s, const char* suffix) {
    unsigned long slen = agn_rt_strlen(s);
    unsigned long suflen = agn_rt_strlen(suffix);
    if (suflen > slen) return 0;
    for (unsigned long i = 0; i < suflen; i++) {
        if (s[slen - suflen + i] != suffix[i]) return 0;
    }
    return 1;
}

extern "C" long agn_rt_char_at(const char* s, long index) {
    unsigned long slen = agn_rt_strlen(s);
    if (index < 0 || static_cast<unsigned long>(index) >= slen) return -1;
    return static_cast<unsigned char>(s[index]);
}

extern "C" char* agn_rt_substr(const char* s, long start, long len) {
    long slen = static_cast<long>(agn_rt_strlen(s));
    if (start < 0) start = 0;
    if (start > slen) start = slen;
    if (len < 0) len = 0;
    if (start + len > slen) len = slen - start;

    char* buf = static_cast<char*>(agn_rt_alloc(static_cast<unsigned long>(len) + 1));
    for (long i = 0; i < len; i++) buf[i] = s[start + i];
    buf[len] = '\0';
    return buf;
}

extern "C" char* agn_rt_to_upper(const char* s) {
    unsigned long len = agn_rt_strlen(s);
    char* buf = static_cast<char*>(agn_rt_alloc(len + 1));
    for (unsigned long i = 0; i < len; i++) buf[i] = toUpperChar(s[i]);
    buf[len] = '\0';
    return buf;
}

extern "C" char* agn_rt_to_lower(const char* s) {
    unsigned long len = agn_rt_strlen(s);
    char* buf = static_cast<char*>(agn_rt_alloc(len + 1));
    for (unsigned long i = 0; i < len; i++) buf[i] = toLowerChar(s[i]);
    buf[len] = '\0';
    return buf;
}
