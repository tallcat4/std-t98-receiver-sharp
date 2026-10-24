#include "pn.hpp"
#include "ambe/bits.hpp"
#include <bit>
#include <stdexcept>

namespace spear::std_t98::secret {

namespace {
constexpr std::array<unsigned, 16> offsets = {
    0, 0x1B95, 0x741B, 0x3679, 0x4D57, 0x55E8, 0x3444, 0x3471,
    0x68A3, 0x7B2C, 0x76FD, 0x7B59, 0x0C53, 0x7769, 0x4597, 0x5748};
}
uint16_t key_for_state(uint16_t state, uint8_t series) {
    if (series > 15 || state > kMaxKey) throw std::invalid_argument("invalid secret series/state");
    if (!state) return 0;
    return static_cast<uint16_t>((state + 32766u - offsets[series]) % 32767u + 1u);
}
Pn196 pn_sequence(uint16_t key, uint8_t series) {
    if (series > 15 || key > kMaxKey) throw std::invalid_argument("invalid secret series/key");
    Pn196 out{};
    unsigned state = key ? (key + offsets[series] - 1u) % 32767u + 1u : 0;
    for (int i = 0; i < kPnBits; ++i) {
        out[static_cast<std::size_t>(i)] = static_cast<uint8_t>(state & 1u);
        const unsigned fb = static_cast<unsigned>(std::popcount(state & kSeriesMasks[series])) & 1u;
        state >>= 1;
        if (fb) state |= 1u << 14;
    }
    return out;
}

Frame49 keystream_thumbdv(const Pn196& pn, int frame) {
    Frame49 ks{};
    for (int i = 0; i < kBitsPerFrame; ++i) ks[static_cast<std::size_t>(i)] = pn[static_cast<std::size_t>(frame * kBitsPerFrame + ambe::kThumbDv[static_cast<std::size_t>(i)])];
    return ks;
}

Frame49 unpack_frame49(const std::array<uint8_t, 7>& payload) {
    Frame49 b{};
    for (int i = 0; i < kBitsPerFrame; ++i) b[static_cast<std::size_t>(i)] = static_cast<uint8_t>((payload[static_cast<std::size_t>(i / 8)] >> (7 - i % 8)) & 1);
    return b;
}

std::array<uint8_t, 7> pack_frame49(const Frame49& bits) {
    std::array<uint8_t, 7> out{};
    for (int i = 0; i < kBitsPerFrame; ++i) out[static_cast<std::size_t>(i / 8)] |= static_cast<uint8_t>((bits[static_cast<std::size_t>(i)] & 1) << (7 - i % 8));
    return out;
}

} // namespace spear::std_t98::secret
