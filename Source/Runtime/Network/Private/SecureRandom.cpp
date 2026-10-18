#include <Network/NetworkTypes.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <stdlib.h>
#else
#include <cerrno>
#include <sys/random.h>
#endif

namespace shockwave::net {

bool secureRandomBytes(std::span<uint8_t> out) {
    if (out.empty()) return true;
#if defined(_WIN32)
    return BCryptGenRandom(nullptr, out.data(), ULONG(out.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#elif defined(__APPLE__)
    arc4random_buf(out.data(), out.size());
    return true;
#else
    size_t filled = 0;
    while (filled < out.size()) {
        const ssize_t got = getrandom(out.data() + filled, out.size() - filled, 0);
        if (got < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        filled += size_t(got);
    }
    return true;
#endif
}

}   // namespace shockwave::net
