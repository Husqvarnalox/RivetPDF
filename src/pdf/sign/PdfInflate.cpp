// SPDX-License-Identifier: MPL-2.0
#include "pdf/sign/PdfInflate.hpp"

#include <array>
#include <cstdlib>

namespace rivet::pdf::sign {

namespace {

SignError malformed(const char* what) { return SignError{SignErrorCode::MalformedPdf, what, std::nullopt}; }

struct Huffman {
    std::array<std::uint16_t, 16> count{};
    std::array<std::uint16_t, 288> symbol{};
};

bool build(Huffman& h, const std::uint8_t* lengths, unsigned n) {
    h.count.fill(0);
    for (unsigned i = 0; i < n; ++i) ++h.count[lengths[i]];
    if (h.count[0] == n) return true; // no codes: valid when unused
    int left = 1;
    for (int len = 1; len <= 15; ++len) {
        left <<= 1;
        left -= h.count[static_cast<std::size_t>(len)];
        if (left < 0) return false; // over-subscribed
    }
    std::array<std::uint16_t, 16> offs{};
    for (int len = 1; len < 15; ++len)
        offs[static_cast<std::size_t>(len + 1)] =
            static_cast<std::uint16_t>(offs[static_cast<std::size_t>(len)] + h.count[static_cast<std::size_t>(len)]);
    for (unsigned i = 0; i < n; ++i)
        if (lengths[i]) h.symbol[offs[lengths[i]]++] = static_cast<std::uint16_t>(i);
    return true;
}

class Inflater {
public:
    Inflater(ByteView in, std::size_t maxOut) : in_(in), maxOut_(maxOut) {}

    SignResult<Bytes> run() {
        if (in_.size() < 2) return std::unexpected(malformed("zlib stream too short"));
        const unsigned cmf = in_[0];
        const unsigned flg = in_[1];
        if ((cmf & 0x0F) != 8 || ((cmf << 8) | flg) % 31 != 0 || (flg & 0x20))
            return std::unexpected(malformed("unsupported zlib header"));
        pos_ = 2;
        bool last = false;
        while (!last) {
            if (!need(3)) return std::unexpected(malformed("truncated deflate stream"));
            last = bits(1) != 0;
            const unsigned type = bits(2);
            std::optional<SignError> err;
            if (type == 0) err = stored();
            else if (type == 1) err = fixed();
            else if (type == 2) err = dynamic();
            else err = malformed("invalid deflate block type");
            if (err) return std::unexpected(*err);
        }
        return std::move(out_);
    }

private:
    bool need(unsigned n) {
        while (bitCount_ < n) {
            if (pos_ >= in_.size()) return false;
            bitBuf_ |= static_cast<std::uint32_t>(in_[pos_++]) << bitCount_;
            bitCount_ += 8;
        }
        return true;
    }
    unsigned bits(unsigned n) {
        const unsigned v = bitBuf_ & ((1u << n) - 1u);
        bitBuf_ >>= n;
        bitCount_ -= n;
        return v;
    }
    int decode(const Huffman& h) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= 15; ++len) {
            if (!need(1)) return -1;
            code |= static_cast<int>(bits(1));
            const int count = h.count[static_cast<std::size_t>(len)];
            if (code - count < first) return h.symbol[static_cast<std::size_t>(index + (code - first))];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }
    std::optional<SignError> put(std::uint8_t b) {
        if (out_.size() >= maxOut_) return SignError{SignErrorCode::LimitExceeded, "inflated stream too large", std::nullopt};
        out_.push_back(b);
        return std::nullopt;
    }

    std::optional<SignError> stored() {
        bitBuf_ = 0;
        bitCount_ = 0;
        if (in_.size() - pos_ < 4) return malformed("truncated stored block");
        const unsigned len = in_[pos_] | (in_[pos_ + 1] << 8);
        const unsigned nlen = in_[pos_ + 2] | (in_[pos_ + 3] << 8);
        pos_ += 4;
        if ((len ^ 0xFFFFu) != nlen) return malformed("corrupt stored block");
        if (in_.size() - pos_ < len) return malformed("truncated stored block");
        if (maxOut_ - out_.size() < len) return SignError{SignErrorCode::LimitExceeded, "inflated stream too large", std::nullopt};
        out_.insert(out_.end(), in_.begin() + static_cast<std::ptrdiff_t>(pos_),
                    in_.begin() + static_cast<std::ptrdiff_t>(pos_ + len));
        pos_ += len;
        return std::nullopt;
    }

    std::optional<SignError> codes(const Huffman& lit, const Huffman& dist) {
        static constexpr std::array<std::uint16_t, 29> kLenBase = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,
                                                                   15, 17, 19, 23, 27, 31, 35, 43, 51,  59,
                                                                   67, 83, 99, 115, 131, 163, 195, 227, 258};
        static constexpr std::array<std::uint8_t, 29> kLenExtra = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                                   2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static constexpr std::array<std::uint16_t, 30> kDistBase = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,
                                                                    33,  49,  65,  97,  129, 193,  257,  385,  513,  769,
                                                                    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static constexpr std::array<std::uint8_t, 30> kDistExtra = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                                    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            const int sym = decode(lit);
            if (sym < 0) return malformed("invalid literal/length code");
            if (sym < 256) {
                if (auto e = put(static_cast<std::uint8_t>(sym))) return e;
            } else if (sym == 256) {
                return std::nullopt;
            } else {
                const unsigned li = static_cast<unsigned>(sym - 257);
                if (li >= 29) return malformed("invalid length symbol");
                if (!need(kLenExtra[li])) return malformed("truncated deflate stream");
                const unsigned len = kLenBase[li] + bits(kLenExtra[li]);
                const int ds = decode(dist);
                if (ds < 0 || ds >= 30) return malformed("invalid distance code");
                if (!need(kDistExtra[static_cast<unsigned>(ds)])) return malformed("truncated deflate stream");
                const unsigned d = kDistBase[static_cast<unsigned>(ds)] + bits(kDistExtra[static_cast<unsigned>(ds)]);
                if (d > out_.size()) return malformed("distance too far back");
                for (unsigned k = 0; k < len; ++k) {
                    if (auto e = put(out_[out_.size() - d])) return e;
                }
            }
        }
    }

    std::optional<SignError> fixed() {
        Huffman lit, dist;
        std::uint8_t lengths[288];
        unsigned i = 0;
        for (; i < 144; ++i) lengths[i] = 8;
        for (; i < 256; ++i) lengths[i] = 9;
        for (; i < 280; ++i) lengths[i] = 7;
        for (; i < 288; ++i) lengths[i] = 8;
        build(lit, lengths, 288);
        std::uint8_t dl[30];
        for (auto& x : dl) x = 5;
        build(dist, dl, 30);
        return codes(lit, dist);
    }

    std::optional<SignError> dynamic() {
        if (!need(14)) return malformed("truncated deflate stream");
        const unsigned nlen = bits(5) + 257;
        const unsigned ndist = bits(5) + 1;
        const unsigned ncode = bits(4) + 4;
        if (nlen > 286 || ndist > 30) return malformed("bad dynamic block counts");
        static constexpr std::array<std::uint8_t, 19> kOrder = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
                                                                11, 4,  12, 3, 13, 2, 14, 1, 15};
        std::uint8_t lengths[320] = {};
        for (unsigned i = 0; i < ncode; ++i) {
            if (!need(3)) return malformed("truncated deflate stream");
            lengths[kOrder[i]] = static_cast<std::uint8_t>(bits(3));
        }
        Huffman lencode;
        if (!build(lencode, lengths, 19)) return malformed("bad code-length code");
        std::uint8_t all[320] = {};
        unsigned idx = 0;
        while (idx < nlen + ndist) {
            const int sym = decode(lencode);
            if (sym < 0) return malformed("bad code-length symbol");
            if (sym < 16) {
                all[idx++] = static_cast<std::uint8_t>(sym);
            } else {
                unsigned rep = 0;
                std::uint8_t val = 0;
                if (sym == 16) {
                    if (idx == 0) return malformed("repeat with no previous length");
                    val = all[idx - 1];
                    if (!need(2)) return malformed("truncated deflate stream");
                    rep = 3 + bits(2);
                } else if (sym == 17) {
                    if (!need(3)) return malformed("truncated deflate stream");
                    rep = 3 + bits(3);
                } else {
                    if (!need(7)) return malformed("truncated deflate stream");
                    rep = 11 + bits(7);
                }
                if (idx + rep > nlen + ndist) return malformed("too many code lengths");
                while (rep--) all[idx++] = val;
            }
        }
        if (all[256] == 0) return malformed("missing end-of-block code");
        Huffman lit, dist;
        if (!build(lit, all, nlen)) return malformed("bad literal/length code");
        if (!build(dist, all + nlen, ndist)) return malformed("bad distance code");
        return codes(lit, dist);
    }

    ByteView in_;
    std::size_t maxOut_;
    std::size_t pos_ = 0;
    std::uint32_t bitBuf_ = 0;
    unsigned bitCount_ = 0;
    Bytes out_;
};

} // namespace

SignResult<Bytes> inflateZlib(ByteView input, std::size_t maxOutput) { return Inflater(input, maxOutput).run(); }

SignResult<Bytes> undoPngPredictor(ByteView data, unsigned columns, unsigned colors) {
    if (columns == 0 || colors == 0 || columns > (1u << 24) || colors > 32)
        return std::unexpected(malformed("bad predictor parameters"));
    const std::size_t rowBytes = static_cast<std::size_t>(columns) * colors;
    const std::size_t stride = rowBytes + 1;
    if (data.size() % stride != 0) return std::unexpected(malformed("predictor data is not a whole number of rows"));
    const std::size_t rows = data.size() / stride;
    Bytes out(rows * rowBytes);
    const std::size_t bpp = colors; // 8-bit components
    for (std::size_t r = 0; r < rows; ++r) {
        const std::uint8_t filter = data[r * stride];
        const std::uint8_t* src = data.data() + r * stride + 1;
        std::uint8_t* dst = out.data() + r * rowBytes;
        const std::uint8_t* prev = r ? out.data() + (r - 1) * rowBytes : nullptr;
        for (std::size_t i = 0; i < rowBytes; ++i) {
            const int a = i >= bpp ? dst[i - bpp] : 0;
            const int b = prev ? prev[i] : 0;
            const int c = (prev && i >= bpp) ? prev[i - bpp] : 0;
            int pred = 0;
            switch (filter) {
            case 0: pred = 0; break;
            case 1: pred = a; break;
            case 2: pred = b; break;
            case 3: pred = (a + b) / 2; break;
            case 4: {
                const int p = a + b - c;
                const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                pred = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                break;
            }
            default: return std::unexpected(malformed("unknown PNG filter"));
            }
            dst[i] = static_cast<std::uint8_t>(src[i] + pred);
        }
    }
    return out;
}

} // namespace rivet::pdf::sign
