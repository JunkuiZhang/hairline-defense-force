#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace hdf {

template <typename T, size_t N> class SpscQueue {
    static_assert(N > 0, "Queue size must be positive");
    static_assert((N & (N - 1)) == 0, "Queue size must be a power of 2");

    struct Slot {
        alignas(T) std::byte storage[sizeof(T)];
    };

  public:
    SpscQueue() {}
    ~SpscQueue() {}

    template <typename U> bool push(U &&item) {
        uint64_t current_tail = tail.load(std::memory_order_relaxed);
        if (current_tail - cached_head >= N) {
            cached_head = head.load(std::memory_order_acquire);
            if (current_tail - cached_head >= N) {
                return false;
            }
        }
        new (&buffer[current_tail & (N - 1)].storage) T(std::forward<U>(item));
        tail.store(current_tail + 1, std::memory_order_release);
        return true;
    }

    bool pop(T &item) {
        uint64_t current_head = head.load(std::memory_order_relaxed);
        if (cached_tail <= current_head) {
            cached_tail = tail.load(std::memory_order_acquire);
            if (cached_tail <= current_head) {
                return false;
            }
        }
        T *ptr = reinterpret_cast<T *>(&buffer[current_head & (N - 1)].storage);
        item = std::move(*ptr);
        ptr->~T();
        head.store(current_head + 1, std::memory_order_release);
        return true;
    }

    size_t size() const {
        uint64_t current_head = head.load(std::memory_order_relaxed);
        uint64_t current_tail = tail.load(std::memory_order_relaxed);
        return current_tail - current_head;
    }

  private:
    alignas(64) std::atomic<uint64_t> head{0};
    uint64_t cached_tail{0};
    alignas(64) std::atomic<uint64_t> tail{0};
    uint64_t cached_head{0};
    alignas(64) std::array<Slot, N> buffer;
};

} // namespace hdf