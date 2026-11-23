// Which file of a GitHub release this launcher updates from: on Windows the launcher .zip
// ("...launcher....zip"), on macOS the packaged RACiT-<version>-macos.zip (mac/package_macos.sh).
// Never the other platform's: a release with both picks this one's, and one with only the other's
// counts as no release for this platform. Takes the name in lower case. No other includes, so a
// test can build it alone.
#pragma once
#include <string>

inline bool ends_with_ci(const std::string& s, const char* tail)
{
    const std::string t(tail);
    return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
}

inline bool release_asset_for(const std::string& low, bool macos)
{
    if (!ends_with_ci(low, ".zip")) return false;
    const bool mac_zip = ends_with_ci(low, "-macos.zip");
    if (macos) return mac_zip;
    return !mac_zip && low.find("launcher") != std::string::npos && low.find("macos") == std::string::npos &&
           low.find("android") == std::string::npos && low.find("linux") == std::string::npos;
}

inline bool release_asset_here(const std::string& low)
{
#ifdef __APPLE__
    return release_asset_for(low, true);
#else
    return release_asset_for(low, false);
#endif
}
