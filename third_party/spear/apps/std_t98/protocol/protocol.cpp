#include "protocol.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace spear::std_t98 {

Symbols quantize(const std::vector<float>& raw) {
    Symbols s(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const float v = raw[i];
        if (v > 2.0f) s[i] = 3;
        else if (v > 0.0f) s[i] = 1;
        else if (v > -2.0f) s[i] = -1;
        else s[i] = -3;
    }
    return s;
}

namespace {
// LFSR 9-bit(初期 [0,1,1,1,0,0,1,0,0]、リスト末尾が s0)。1 = そのシンボルの符号を反転
const std::array<int, 182>& whitening_seq() {
    static const std::array<int, 182> w = [] {
        std::array<int, 9> lfsr = {0, 1, 1, 1, 0, 0, 1, 0, 0};
        std::array<int, 182> out{};
        for (int i = 0; i < 182; ++i) {
            const int s0 = lfsr[8];
            const int s4 = lfsr[4];
            const int new_s8 = s4 ^ s0;
            out[i] = s0;
            // lfsr = [new_s8] + lfsr[:-1]
            for (int j = 8; j > 0; --j) lfsr[j] = lfsr[j - 1];
            lfsr[0] = new_s8;
        }
        return out;
    }();
    return w;
}
}

Symbols dewhiten(const Symbols& sym) {
    if (sym.size() < 192) return sym;
    const auto& whitening = whitening_seq();
    Symbols out = sym;
    for (int i = 0; i < 182; ++i) {
        const int sign = whitening[i] == 0 ? 1 : -1;
        out[10 + i] = static_cast<int8_t>(out[10 + i] * sign);
    }
    return out;
}

std::string symbols_to_bits(const Symbols& sym) {
    // symbols_to_bit_values: +1→0, +3→1, -1→2, -3→3 → 各 2 bit
    std::string bits;
    bits.reserve(sym.size() * 2);
    for (int8_t s : sym) {
        uint8_t v = 0;
        if (s == -3) v = 3;
        else if (s == -1) v = 2;
        else if (s == 1) v = 0;
        else if (s == 3) v = 1;
        bits.push_back(static_cast<char>('0' + ((v >> 1) & 1)));
        bits.push_back(static_cast<char>('0' + (v & 1)));
    }
    return bits;
}

FrameFields split_frame(const std::string& bits) {
    FrameFields f;
    std::size_t p = 0;
    auto take = [&](int n) { std::string s = bits.substr(p, n); p += n; return s; };
    f.sw = take(20);
    f.rich = take(16);
    f.sacch = take(60);
    f.tch1 = take(144);
    f.tch2 = take(144);
    return f;
}

FrameFields parse_frame(const Symbols& dewhitened) {
    return split_frame(symbols_to_bits(dewhitened));
}

Rich decode_rich(const std::string& rich_bits) {
    Rich r;
    if (rich_bits.size() != 16) return r;
    std::array<int, 8> decoded{};
    for (int i = 0; i < 8; ++i) {
        const std::string pair = rich_bits.substr(i * 2, 2);
        if (pair == "00" || pair == "01") decoded[i] = 0;
        else if (pair == "10" || pair == "11") decoded[i] = 1;
        else return r;  // invalid
    }
    r.valid = true;
    r.f = decoded[0];
    r.res1 = decoded[1];
    r.res2 = decoded[2];
    r.m = (decoded[3] << 2) | (decoded[4] << 1) | decoded[5];
    r.d = decoded[6];
    r.parity = decoded[7];
    int calc = 0;
    for (int i = 0; i < 7; ++i) calc += decoded[i];
    r.parity_ok = (calc % 2) == r.parity;
    return r;
}

namespace {

// 畳み込み符号 K=5 のトレリス(SACCH/PICH 共通)
struct Trellis {
    std::array<std::array<int, 2>, 16> next_state{};
    std::array<std::array<int, 2>, 16> g1{}, g2{};
    Trellis() {
        for (int state = 0; state < 16; ++state) {
            for (int bit = 0; bit < 2; ++bit) {
                const int x_t = bit;
                const int x_t1 = (state >> 3) & 1;
                const int x_t2 = (state >> 2) & 1;
                const int x_t3 = (state >> 1) & 1;
                const int x_t4 = state & 1;
                next_state[state][bit] = (bit << 3) | (state >> 1);
                g1[state][bit] = x_t ^ x_t3 ^ x_t4;
                g2[state][bit] = x_t ^ x_t1 ^ x_t2 ^ x_t4;
            }
        }
    }
};
const Trellis& trellis() { static const Trellis t; return t; }

// Viterbi 復号(rate 1/2、状態 0 で終端)。depunctured は {0,1,-1} で -1 = punctured(無視)。
// steps ステップ、最後の terminate_steps は bit=0 に固定。戻り値: 復号ビット列と累積コスト。
struct ViterbiResult { std::vector<int> bits; int cost; };
ViterbiResult viterbi(const std::vector<int>& depunctured, int steps, int free_steps) {
    const Trellis& tr = trellis();
    constexpr int INF = std::numeric_limits<int>::max() / 2;
    std::array<int, 16> cost;
    std::array<std::vector<int>, 16> hist;
    cost.fill(INF);
    cost[0] = 0;
    for (int step = 0; step < steps; ++step) {
        const int r1 = depunctured[step * 2];
        const int r2 = depunctured[step * 2 + 1];
        std::array<int, 16> ncost;
        std::array<std::vector<int>, 16> nhist;
        ncost.fill(INF);
        const int max_bit = step < free_steps ? 2 : 1;
        for (int state = 0; state < 16; ++state) {
            if (cost[state] >= INF) continue;
            for (int bit = 0; bit < max_bit; ++bit) {
                const int ns = tr.next_state[state][bit];
                int dist = 0;
                if (r1 != -1 && r1 != tr.g1[state][bit]) ++dist;
                if (r2 != -1 && r2 != tr.g2[state][bit]) ++dist;
                const int nc = cost[state] + dist;
                if (nc < ncost[ns]) {
                    ncost[ns] = nc;
                    nhist[ns] = hist[state];
                    nhist[ns].push_back(bit);
                }
            }
        }
        cost = ncost;
        hist = std::move(nhist);
    }
    return {hist[0], cost[0]};
}

std::string bits_to_str(const std::vector<int>& bits, int begin, int end) {
    std::string s;
    for (int i = begin; i < end; ++i) s.push_back(static_cast<char>('0' + bits[i]));
    return s;
}
int bits_to_int(const std::vector<int>& bits, int begin, int end) {
    int v = 0;
    for (int i = begin; i < end; ++i) v = (v << 1) | bits[i];
    return v;
}

} // namespace

Sacch decode_sacch(const std::string& sacch_bits) {
    Sacch out;
    if (sacch_bits.size() != 60) return out;
    std::array<int, 60> bits{};
    for (int i = 0; i < 60; ++i) bits[i] = sacch_bits[i] - '0';

    // deinterleave 5x12
    std::array<int, 60> deint{};
    for (int col = 0; col < 12; ++col)
        for (int row = 0; row < 5; ++row)
            deint[row * 12 + col] = bits[col * 5 + row];

    // depuncture: 6 グループ、各 6 入力から 8 出力(index 2,5 の後に punctured 挿入)
    std::vector<int> dep;
    int idx = 0;
    for (int g = 0; g < 6; ++g) {
        for (int pi = 0; pi < 6; ++pi) {
            dep.push_back(deint[idx++]);
            if (pi == 2 || pi == 5) dep.push_back(-1);   // punctured
            else dep.push_back(deint[idx++]);
        }
    }
    // 36 ステップ、最後 4 は bit=0 固定(free_steps=32)
    auto vr = viterbi(dep, 36, 32);
    const auto& decoded = vr.bits;
    out.bit_errors = vr.cost;

    // msg 26 bit + CRC 6 bit
    std::vector<int> msg(decoded.begin(), decoded.begin() + 26);
    std::vector<int> crc_recv(decoded.begin() + 26, decoded.begin() + 32);

    // CRC-6(register 初期全 1、feedback = bit ^ reg[5])
    std::array<int, 6> reg = {1, 1, 1, 1, 1, 1};
    for (int bit : msg) {
        const int fb = bit ^ reg[5];
        std::array<int, 6> nreg = {fb, reg[0] ^ fb, reg[1] ^ fb, reg[2], reg[3], reg[4] ^ fb};
        reg = nreg;
    }
    std::array<int, 6> crc_calc = {reg[5], reg[4], reg[3], reg[2], reg[1], reg[0]};
    bool ok = std::equal(crc_calc.begin(), crc_calc.end(), crc_recv.begin());
    if (!ok) {
        std::array<int, 6> rev = {reg[0], reg[1], reg[2], reg[3], reg[4], reg[5]};
        if (std::equal(rev.begin(), rev.end(), crc_recv.begin())) { ok = true; crc_calc = rev; }
    }
    out.crc_ok = ok;
    out.f = msg[0];
    out.wr = bits_to_int(msg, 1, 3);
    out.msg_type = bits_to_int(msg, 3, 8);
    out.call_stat = bits_to_int(msg, 8, 10);
    out.user_code = bits_to_int(msg, 10, 19);
    out.maker_code = bits_to_int(msg, 19, 26);
    for (int b : crc_recv) out.crc_recv.push_back(static_cast<char>('0' + b));
    for (int b : crc_calc) out.crc_calc.push_back(static_cast<char>('0' + b));
    return out;
}

Pich decode_pich(const std::string& pich_bits) {
    Pich out;
    if (pich_bits.size() != 144) return out;
    std::array<int, 144> bits{};
    for (int i = 0; i < 144; ++i) bits[i] = pich_bits[i] - '0';

    // deinterleave 9x16
    std::array<int, 144> deint{};
    for (int col = 0; col < 16; ++col)
        for (int row = 0; row < 9; ++row)
            deint[row * 16 + col] = bits[col * 9 + row];

    // depuncture: 48 グループ、各 (a, punctured, b, c)
    std::vector<int> dep;
    int idx = 0;
    for (int g = 0; g < 48; ++g) {
        dep.push_back(deint[idx++]);
        dep.push_back(-1);
        dep.push_back(deint[idx++]);
        dep.push_back(deint[idx++]);
    }
    auto vr = viterbi(dep, 96, 92);
    const auto& decoded = vr.bits;
    out.bit_errors = vr.cost;

    std::vector<int> msg(decoded.begin(), decoded.begin() + 80);
    std::vector<int> crc_recv(decoded.begin() + 80, decoded.begin() + 92);

    // CRC-12
    std::array<int, 12> reg;
    reg.fill(1);
    for (int bit : msg) {
        const int fb = bit ^ reg[11];
        std::array<int, 12> n{};
        n[0] = fb;
        n[1] = reg[0] ^ fb;
        n[2] = reg[1] ^ fb;
        n[3] = reg[2] ^ fb;
        n[4] = reg[3];
        n[5] = reg[4];
        n[6] = reg[5];
        n[7] = reg[6];
        n[8] = reg[7];
        n[9] = reg[8];
        n[10] = reg[9];
        n[11] = reg[10] ^ fb;
        reg = n;
    }
    std::array<int, 12> crc_calc;
    for (int i = 0; i < 12; ++i) crc_calc[i] = reg[11 - i];
    bool ok = std::equal(crc_calc.begin(), crc_calc.end(), crc_recv.begin());
    if (!ok && std::equal(reg.begin(), reg.end(), crc_recv.begin())) { ok = true; for (int i = 0; i < 12; ++i) crc_calc[i] = reg[i]; }
    out.crc_ok = ok;

    // CSM: msg[0:36] を 9 個の 4-bit hex 桁
    const char* hex = "0123456789ABCDEF";
    for (int i = 0; i < 9; ++i) out.csm.push_back(hex[bits_to_int(msg, i * 4, i * 4 + 4)]);
    out.reserve = bits_to_str(msg, 36, 80);
    for (int b : crc_recv) out.crc_recv.push_back(static_cast<char>('0' + b));
    for (int b : crc_calc) out.crc_calc.push_back(static_cast<char>('0' + b));
    return out;
}

std::array<std::string, 4> split_traffic_blocks(const FrameFields& f) {
    return {f.tch1.substr(0, 72), f.tch1.substr(72), f.tch2.substr(0, 72), f.tch2.substr(72)};
}

std::vector<int8_t> traffic_llr(const std::vector<float>& raw) {
    std::vector<int8_t> llr;
    if (raw.size() < 192) return llr;
    llr.reserve(288);
    const auto& w = whitening_seq();
    auto sat = [](float v) { return static_cast<int8_t>(std::lround(std::clamp(v * kLlrScale, -127.f, 127.f))); };
    for (int k = 48; k < 192; ++k) {   // TCH1(シンボル 48..119)+ TCH2(120..191)
        const float y = w[static_cast<std::size_t>(k - 10)] ? -raw[static_cast<std::size_t>(k)] : raw[static_cast<std::size_t>(k)];
        llr.push_back(sat(-y));
        llr.push_back(sat(std::fabs(y) - 2.f));
    }
    return llr;
}

std::vector<uint8_t> traffic_blocks_to_payload(const std::array<std::string, 4>& blocks) {
    std::vector<uint8_t> out;
    out.reserve(4 * 9);
    for (const auto& b : blocks) {
        // 72 bit → 9 bytes(big-endian)
        for (int byte = 0; byte < 9; ++byte) {
            uint8_t v = 0;
            for (int bit = 0; bit < 8; ++bit) v = static_cast<uint8_t>((v << 1) | (b[byte * 8 + bit] - '0'));
            out.push_back(v);
        }
    }
    return out;
}

} // namespace spear::std_t98
