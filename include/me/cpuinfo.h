// cpuinfo.h — best-effort CPU brand string for the benchmark methodology
// (Section 9.4: report the exact machine). Uses CPUID on x86; "unknown"
// elsewhere.
#ifndef ME_CPUINFO_H
#define ME_CPUINFO_H

#include <cstring>
#include <string>

#if defined(_MSC_VER)
#  include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#  include <cpuid.h>
#endif

namespace me {

inline std::string cpu_brand() {
#if defined(_MSC_VER)
    int regs[4];
    char brand[49] = {0};
    __cpuid(regs, 0x80000000);
    if (static_cast<unsigned>(regs[0]) >= 0x80000004u) {
        for (unsigned i = 0; i < 3; ++i) {
            __cpuid(regs, 0x80000002 + i);
            std::memcpy(brand + i * 16, regs, 16);
        }
        return std::string(brand);
    }
    return "unknown";
#elif defined(__x86_64__) || defined(__i386__)
    unsigned a, b, c, d;
    char brand[49] = {0};
    if (__get_cpuid(0x80000000u, &a, &b, &c, &d) && a >= 0x80000004u) {
        for (unsigned i = 0; i < 3; ++i) {
            __get_cpuid(0x80000002u + i, &a, &b, &c, &d);
            std::memcpy(brand + i * 16 + 0,  &a, 4);
            std::memcpy(brand + i * 16 + 4,  &b, 4);
            std::memcpy(brand + i * 16 + 8,  &c, 4);
            std::memcpy(brand + i * 16 + 12, &d, 4);
        }
        return std::string(brand);
    }
    return "unknown";
#else
    return "unknown";
#endif
}

}  // namespace me

#endif  // ME_CPUINFO_H
