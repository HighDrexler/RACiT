// What a game or launcher program starts with: a Windows program "MZ", a macOS one a Mach-O
// header (64-bit, either byte order) or a universal ("fat") one. An update checks this before
// it trusts a release's build or launcher. No other includes, so a test can build it alone.
#pragma once
#include <cstddef>

inline bool looks_like_windows_program(const unsigned char* h, size_t n)
{
    return n >= 2 && h[0] == 'M' && h[1] == 'Z';
}

inline bool looks_like_macos_program(const unsigned char* h, size_t n)
{
    if (n < 4) return false;
    const unsigned long m = (unsigned long)h[0] << 24 | (unsigned long)h[1] << 16 | (unsigned long)h[2] << 8 | h[3];
    return m == 0xFEEDFACFul || m == 0xCFFAEDFEul    // 64-bit, big- or little-endian
        || m == 0xCAFEBABEul || m == 0xCAFEBABFul;   // universal (fat, fat64)
}

inline bool looks_like_program_head(const unsigned char* h, size_t n)
{
#ifdef __APPLE__
    return looks_like_macos_program(h, n);
#else
    return looks_like_windows_program(h, n);
#endif
}
