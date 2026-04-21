#pragma once

#include <algorithm>
#include <ostream>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>

namespace hdf {

/**
 * @brief 定长字符串：内联存储，零堆分配。
 *
 * 用于替换热路径上的 std::string，消除小字符串的拷贝构造和堆分配开销。
 * 存储为 null-terminated char 数组，支持与 std::string / std::string_view
 * 的隐式互操作。
 *
 * @tparam N 最大容量（不含 '\0'），实际存储 N+1 字节。
 */
template <size_t N>
class FixedString {
public:
    FixedString() noexcept { data_[0] = '\0'; }

    FixedString(const char* s) noexcept {
        if (s) {
            size_t len = std::strlen(s);
            if (len > N) len = N;
            std::memcpy(data_, s, len);
            data_[len] = '\0';
            size_ = static_cast<uint8_t>(len);
        } else {
            data_[0] = '\0';
            size_ = 0;
        }
    }

    FixedString(const std::string& s) noexcept {
        size_t len = s.size();
        if (len > N) len = N;
        std::memcpy(data_, s.data(), len);
        data_[len] = '\0';
        size_ = static_cast<uint8_t>(len);
    }

    FixedString(std::string_view sv) noexcept {
        size_t len = sv.size();
        if (len > N) len = N;
        std::memcpy(data_, sv.data(), len);
        data_[len] = '\0';
        size_ = static_cast<uint8_t>(len);
    }

    // 赋值
    FixedString& operator=(const std::string& s) noexcept {
        size_t len = s.size();
        if (len > N) len = N;
        std::memcpy(data_, s.data(), len);
        data_[len] = '\0';
        size_ = static_cast<uint8_t>(len);
        return *this;
    }

    FixedString& operator=(const char* s) noexcept {
        if (s) {
            size_t len = std::strlen(s);
            if (len > N) len = N;
            std::memcpy(data_, s, len);
            data_[len] = '\0';
            size_ = static_cast<uint8_t>(len);
        } else {
            data_[0] = '\0';
            size_ = 0;
        }
        return *this;
    }

    FixedString& operator=(std::string_view sv) noexcept {
        size_t len = sv.size();
        if (len > N) len = N;
        std::memcpy(data_, sv.data(), len);
        data_[len] = '\0';
        size_ = static_cast<uint8_t>(len);
        return *this;
    }

    // 隐式转换
    operator std::string() const { return std::string(data_, size_); }
    operator std::string_view() const noexcept { return std::string_view(data_, size_); }

    // 访问
    const char* c_str() const noexcept { return data_; }
    const char* data() const noexcept { return data_; }
    size_t size() const noexcept { return size_; }
    size_t length() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

    // 比较
    bool operator==(const FixedString& other) const noexcept {
        return size_ == other.size_ && std::memcmp(data_, other.data_, size_) == 0;
    }
    bool operator!=(const FixedString& other) const noexcept {
        return !(*this == other);
    }
    bool operator==(const std::string& s) const noexcept {
        return size_ == s.size() && std::memcmp(data_, s.data(), size_) == 0;
    }
    bool operator!=(const std::string& s) const noexcept {
        return !(*this == s);
    }
    bool operator==(const char* s) const noexcept {
        return std::strcmp(data_, s) == 0;
    }
    bool operator!=(const char* s) const noexcept {
        return !(*this == s);
    }
    bool operator==(std::string_view sv) const noexcept {
        return std::string_view(data_, size_) == sv;
    }
    bool operator<(const FixedString& other) const noexcept {
        int r = std::memcmp(data_, other.data_, std::min(size_, other.size_));
        if (r != 0) return r < 0;
        return size_ < other.size_;
    }

    // 拼接（返回 std::string，用于非热路径）
    friend std::string operator+(const std::string& lhs, const FixedString& rhs) {
        return lhs + std::string(rhs.data_, rhs.size_);
    }
    friend std::string operator+(const FixedString& lhs, const std::string& rhs) {
        return std::string(lhs.data_, lhs.size_) + rhs;
    }

    // 流输出
    friend std::ostream& operator<<(std::ostream& os, const FixedString& fs) {
        return os.write(fs.data_, fs.size_);
    }

private:
    char data_[N + 1];
    uint8_t size_ = 0;
};

} // namespace hdf

// std::hash 特化，使 FixedString 可用于 unordered_map
namespace std {
template <size_t N>
struct hash<hdf::FixedString<N>> {
    size_t operator()(const hdf::FixedString<N>& fs) const noexcept {
        return std::hash<std::string_view>{}(std::string_view(fs.data(), fs.size()));
    }
};
} // namespace std
