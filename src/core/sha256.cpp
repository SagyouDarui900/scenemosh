// sha256.cpp — see sha256.hpp.
//
// Windows: BCrypt. Other platforms: FIPS 180-4 SHA-256 implemented here.

#include "sha256.hpp"

#include <array>
#include <fstream>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#ifdef _MSC_VER
#pragma comment(lib, "bcrypt.lib")
#endif
#endif

namespace hybrid {

namespace {

const char* kHex = "0123456789abcdef";

std::string to_hex(std::span<const std::uint8_t> digest) {
    std::string out;
    out.reserve(digest.size() * 2);
    for (std::uint8_t b : digest) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0xF]);
    }
    return out;
}

#ifndef _WIN32
// ---- FIPS 180-4 SHA-256 (portable implementation) ----
constexpr std::array<std::uint32_t, 64> kK = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

constexpr std::uint32_t rotr(std::uint32_t x, int n) noexcept {
    return (x >> n) | (x << (32 - n));
}

std::array<std::uint8_t, 32> sha256_impl(std::span<const std::uint8_t> data) {
    std::array<std::uint32_t, 8> h = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u,
                                      0xa54ff53au, 0x510e527fu, 0x9b05688cu,
                                      0x1f83d9abu, 0x5be0cd19u};
    const std::uint64_t bit_len = static_cast<std::uint64_t>(data.size()) * 8;
    std::vector<std::uint8_t> msg(data.begin(), data.end());
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 7; i >= 0; --i) {
        msg.push_back(static_cast<std::uint8_t>((bit_len >> (i * 8)) & 0xFF));
    }
    for (std::size_t off = 0; off < msg.size(); off += 64) {
        std::array<std::uint32_t, 64> w{};
        for (int i = 0; i < 16; ++i) {
            w[static_cast<std::size_t>(i)] =
                (static_cast<std::uint32_t>(msg[off + static_cast<std::size_t>(i) * 4]) << 24) |
                (static_cast<std::uint32_t>(msg[off + static_cast<std::size_t>(i) * 4 + 1]) << 16) |
                (static_cast<std::uint32_t>(msg[off + static_cast<std::size_t>(i) * 4 + 2]) << 8) |
                static_cast<std::uint32_t>(msg[off + static_cast<std::size_t>(i) * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[static_cast<std::size_t>(i - 15)], 7) ^
                                     rotr(w[static_cast<std::size_t>(i - 15)], 18) ^
                                     (w[static_cast<std::size_t>(i - 15)] >> 3);
            const std::uint32_t s1 = rotr(w[static_cast<std::size_t>(i - 2)], 17) ^
                                     rotr(w[static_cast<std::size_t>(i - 2)], 19) ^
                                     (w[static_cast<std::size_t>(i - 2)] >> 10);
            w[static_cast<std::size_t>(i)] = w[static_cast<std::size_t>(i - 16)] + s0 +
                                             w[static_cast<std::size_t>(i - 7)] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        std::uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ ((~e) & g);
            const std::uint32_t t1 = hh + s1 + ch + kK[static_cast<std::size_t>(i)] +
                                     w[static_cast<std::size_t>(i)];
            const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = s0 + maj;
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    std::array<std::uint8_t, 32> out{};
    for (int i = 0; i < 8; ++i) {
        out[static_cast<std::size_t>(i) * 4] = static_cast<std::uint8_t>(h[static_cast<std::size_t>(i)] >> 24);
        out[static_cast<std::size_t>(i) * 4 + 1] = static_cast<std::uint8_t>(h[static_cast<std::size_t>(i)] >> 16);
        out[static_cast<std::size_t>(i) * 4 + 2] = static_cast<std::uint8_t>(h[static_cast<std::size_t>(i)] >> 8);
        out[static_cast<std::size_t>(i) * 4 + 3] = static_cast<std::uint8_t>(h[static_cast<std::size_t>(i)]);
    }
    return out;
}
#endif

} // namespace

std::string sha256_hex(std::span<const std::uint8_t> data) {
#ifdef _WIN32
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        return {};
    }
    BCRYPT_HASH_HANDLE hash = nullptr;
    NTSTATUS status = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
    if (status != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }
    std::array<std::uint8_t, 32> digest{};
    status = BCryptHashData(hash, const_cast<PUCHAR>(data.data()),
                            static_cast<ULONG>(data.size()), 0);
    if (status == 0) {
        status = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (status != 0) return {};
    return to_hex(digest);
#else
    return to_hex(sha256_impl(data));
#endif
}

std::string sha256_hex(std::string_view text) {
    return sha256_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

std::string sha256_file_hex(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
#ifdef _WIN32
    // Stream through BCrypt via the buffer path.
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    return sha256_hex(bytes);
#else
    // Stream block by block through the portable implementation by
    // hashing the whole file content in memory (files here are ~40 MB).
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    return sha256_hex(bytes);
#endif
}

} // namespace hybrid
