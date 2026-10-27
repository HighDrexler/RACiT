// A PS3 disc image (.iso): ISO 9660 (Joliet names when the image has them), read
// straight from the file -- nothing is mounted. Two kinds of image:
//   decrypted (3k3y, ManaGunZ, multiMAN, PS3 Disc Dumper's .iso): files read as they are;
//   Redump (encrypted): sector 0 lists the encrypted sector ranges, each sector is
//   AES-128-CBC under the disc's key (the 16-byte .dkey Redump gives with the dump),
//   IV = the sector number, big-endian. The launcher ships no key: the player's .dkey.
#pragma once
#include "util.h"

struct IsoFile {
    std::string path;                            // "PS3_GAME/USRDIR/EBOOT.BIN"
    uint64_t size = 0;
    std::vector<std::pair<uint32_t, uint32_t>> extents;   // (first sector, bytes); >4 GB files have several
};

class IsoImage {
public:
    ~IsoImage();
    // "" = opened and its file list read.
    std::string open(const fs::path& iso);
    const std::vector<IsoFile>& files() const { return files_; }
    const IsoFile* find(const std::string& path) const;   // case-insensitive, '/' separated
    bool has_encrypted_regions() const { return !enc_.empty(); }
    // The 16-byte disc key (32 hex digits, a .dkey file's text). "" = taken.
    std::string set_key(const std::string& hex);
    bool keyed() const { return key_ != nullptr; }
    // A file's bytes (decrypted when the image is encrypted and a key is set).
    std::string read(const IsoFile& f, uint64_t max = 1u << 20);
    // Writes f to dst, adding to p->done. "" = done.
    std::string extract(const IsoFile& f, const fs::path& dst, Progress* p);

private:
    bool read_sectors(uint32_t first, uint32_t count, unsigned char* out);
    void decrypt(uint32_t first, uint32_t count, unsigned char* buf);
    void walk(uint32_t sector, uint32_t bytes, const std::string& prefix, bool joliet, int depth);

    void* f_ = nullptr;                          // HANDLE
    void* alg_ = nullptr;                        // BCRYPT_ALG_HANDLE
    void* key_ = nullptr;                        // BCRYPT_KEY_HANDLE
    std::vector<std::pair<uint32_t, uint32_t>> enc_;   // encrypted sectors [first, last]
    std::vector<IsoFile> files_;
};

// A .dkey beside the image: "<image name>.dkey", else the only .dkey in its folder. "" = none.
std::string find_dkey(const fs::path& iso, fs::path* found = nullptr);
