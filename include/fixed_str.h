#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>

namespace hdf {

/**
 * @brief 定长栈上字符串，零堆分配。
 *
 * 用于替代 std::string，使包含它的 struct 成为 trivially copyable，
 * 适合在 SPSC 队列等性能敏感路径上传递。
 *
 * @tparam N 缓冲区大小（含 '\0' 终止符，可用字符数为 N-1）
 */
template <size_t N> struct FixedStr {
    static_assert(N > 0, "FixedStr size must be positive");

    char data[N] = {};

    FixedStr() = default;

    // NOLINTNEXTLINE(google-explicit-constructor)
    FixedStr(const char *s) {
        if (s)
            std::strncpy(data, s, N - 1);
    }

    // NOLINTNEXTLINE(google-explicit-constructor)
    FixedStr(const std::string &s) { std::strncpy(data, s.c_str(), N - 1); }

    // NOLINTNEXTLINE(google-explicit-constructor)
    FixedStr(std::string_view s) {
        auto n = std::min(s.size(), N - 1);
        std::memcpy(data, s.data(), n);
    }

    // ─── 转换 ─────────────────────────────────────────────

    // NOLINTNEXTLINE(google-explicit-constructor)
    operator std::string_view() const { return {data, std::strlen(data)}; }

    std::string str() const { return std::string(data); }

    const char *c_str() const noexcept { return data; }

    size_t size() const noexcept { return std::strlen(data); }

    bool empty() const noexcept { return data[0] == '\0'; }

    // ─── 比较 ─────────────────────────────────────────────

    bool operator==(const FixedStr &o) const {
        return std::strcmp(data, o.data) == 0;
    }
    bool operator!=(const FixedStr &o) const { return !(*this == o); }
    bool operator<(const FixedStr &o) const {
        return std::strcmp(data, o.data) < 0;
    }

    bool operator==(const char *s) const { return std::strcmp(data, s) == 0; }
    bool operator==(const std::string &s) const {
        return std::strcmp(data, s.c_str()) == 0;
    }
    bool operator==(std::string_view s) const {
        return std::string_view(data, std::strlen(data)) == s;
    }

    // ─── 赋值 ─────────────────────────────────────────────

    FixedStr &operator=(const char *s) {
        std::memset(data, 0, N);
        if (s)
            std::strncpy(data, s, N - 1);
        return *this;
    }

    FixedStr &operator=(const std::string &s) {
        std::memset(data, 0, N);
        std::strncpy(data, s.c_str(), N - 1);
        return *this;
    }

    FixedStr &operator=(std::string_view s) {
        std::memset(data, 0, N);
        auto n = std::min(s.size(), N - 1);
        std::memcpy(data, s.data(), n);
        return *this;
    }

    // ─── 拼接 ─────────────────────────────────────────────

    FixedStr &append(const char *s) {
        size_t len = std::strlen(data);
        if (len < N - 1 && s) {
            std::strncpy(data + len, s, N - 1 - len);
        }
        return *this;
    }

    FixedStr &append(std::string_view s) {
        size_t len = std::strlen(data);
        if (len < N - 1) {
            auto n = std::min(s.size(), N - 1 - len);
            std::memcpy(data + len, s.data(), n);
        }
        return *this;
    }
};

// ─── ostream ──────────────────────────────────────────────

template <size_t N>
std::ostream &operator<<(std::ostream &os, const FixedStr<N> &s) {
    return os << s.data;
}

// ─── 反向比较（std::string == FixedStr）─────────────────

template <size_t N>
bool operator==(const std::string &lhs, const FixedStr<N> &rhs) {
    return rhs == lhs;
}

} // namespace hdf

// ─── std::hash 特化 ───────────────────────────────────────

template <size_t N> struct std::hash<hdf::FixedStr<N>> {
    size_t operator()(const hdf::FixedStr<N> &s) const noexcept {
        return std::hash<std::string_view>{}(std::string_view(s));
    }
};
