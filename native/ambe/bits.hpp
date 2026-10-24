// AMBE+2 2450 の 49 bit の並び(mbelib の ambe_d 順 ⇔ ThumbDV / 2450 ペイロード順)。
// 秘話の PN(core/crypto/secret_voice.py の THUMBDV_MAP)も同じ表なので、PN は d 順にそのまま XOR できる(secret/pn.hpp)。
// 表は blip25c(blip25.c の RAW_TO_AMBE_D)と同じ。
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace spear::std_t98::ambe {

using Bits49 = std::array<uint8_t, 49>;   // mbelib の ambe_d 順(u0 の MSB が [0])。1 要素 1 ビット

// ThumbDV bit i = d[kThumbDv[i]]
inline constexpr std::array<int, 49> kThumbDv = {0, 18, 36, 1, 19, 37, 2, 20, 38, 3, 21, 39, 4, 22, 40, 5, 23, 41, 6, 24, 42, 7, 25, 43, 8, 26, 44,
                                                 9, 27, 45, 10, 28, 46, 11, 29, 47, 12, 30, 48, 13, 31, 14, 32, 15, 33, 16, 34, 17, 35};

inline std::array<uint8_t, 7> pack_thumbdv(const Bits49& d) {
    std::array<uint8_t, 7> out{};
    for (int i = 0; i < 49; ++i) out[static_cast<std::size_t>(i / 8)] |= static_cast<uint8_t>((d[static_cast<std::size_t>(kThumbDv[static_cast<std::size_t>(i)])] & 1) << (7 - i % 8));
    return out;
}

inline Bits49 unpack_thumbdv(std::span<const uint8_t, 7> payload) {
    Bits49 d{};
    for (int i = 0; i < 49; ++i) d[static_cast<std::size_t>(kThumbDv[static_cast<std::size_t>(i)])] = static_cast<uint8_t>((payload[static_cast<std::size_t>(i / 8)] >> (7 - i % 8)) & 1);
    return d;
}

} // namespace spear::std_t98::ambe
