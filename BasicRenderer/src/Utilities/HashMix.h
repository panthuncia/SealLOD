#pragma once
#include <cstdint>
#include <functional>
#include <type_traits>

namespace util {

    // SplitMix64 finalizer (quality > speed; great for combining bits)
    constexpr uint64_t mix64(uint64_t x) noexcept {
        x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ull;
        x ^= x >> 27; x *= 0x94d049bb133111ebull;
        x ^= x >> 31;
        return x;
    }

    template<class T>
    constexpr uint64_t to_u64(const T& v) noexcept {
        if constexpr (std::is_enum_v<T>) {
            return static_cast<uint64_t>(v);
        }
        else if constexpr (std::is_integral_v<T> && sizeof(T) <= sizeof(uint64_t)) {
            return static_cast<uint64_t>(v);
        }
        else {
            // Fall back to std::hash for arbitrary types
            return static_cast<uint64_t>(std::hash<T>{}(v));
        }
    }

    template<class T>
    constexpr void hash_combine_u64(uint64_t& seed, const T& v) noexcept {
        constexpr uint64_t kPhi = 0x9e3779b97f4a7c15ull; // golden ratio
        uint64_t h = mix64(to_u64(v));
        seed ^= h + kPhi + (seed << 6) + (seed >> 2);
    }

    template<class... Ts>
    constexpr size_t hash_mix(const Ts&... xs) noexcept {
        uint64_t seed = 0;
        (hash_combine_u64(seed, xs), ...);
        return static_cast<size_t>(seed);
    }

} // namespace util
