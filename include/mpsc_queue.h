#pragma once

#include <atomic>
#include <cstddef>
#include <thread>
#include <utility>

namespace hdf {

/**
 * @brief 基于序列号的经典无锁有界 MPSC / MPMC 队列 (Dmitry Vyukov Bounded Queue)
 * 采用环形数组 (Ring Buffer) 结合 Cache Line 对齐，消除伪共享。
 */
template <typename T, size_t Capacity>
class MPSCQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");
    static constexpr size_t Mask = Capacity - 1;

    struct alignas(64) Slot {
        std::atomic<size_t> sequence;
        T data;
    };

    alignas(64) Slot slots_[Capacity];
    alignas(64) std::atomic<size_t> enqueuePos_{0};
    alignas(64) size_t dequeuePos_{0}; // Single consumer, no need for atomic

public:
    MPSCQueue() {
        for (size_t i = 0; i < Capacity; ++i) {
            slots_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    ~MPSCQueue() = default;

    /**
     * @brief 阻塞（Yield）直到成功 Push
     */
    void push(T data) {
        Slot* slot = nullptr;
        size_t pos = enqueuePos_.load(std::memory_order_relaxed);
        while (true) {
            slot = &slots_[pos & Mask];
            size_t seq = slot->sequence.load(std::memory_order_acquire);
            intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            if (dif == 0) {
                // 占位成功
                if (enqueuePos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (dif < 0) {
                // 队列已满，Yield
                std::this_thread::yield();
                pos = enqueuePos_.load(std::memory_order_relaxed);
            } else {
                pos = enqueuePos_.load(std::memory_order_relaxed);
            }
        }
        slot->data = std::move(data);
        slot->sequence.store(pos + 1, std::memory_order_release);
    }

    /**
     * @brief 尝试获取数据，非阻塞，失败返回 false
     */
    bool pop(T& data) {
        size_t pos = dequeuePos_;
        Slot* slot = &slots_[pos & Mask];
        size_t seq = slot->sequence.load(std::memory_order_acquire);
        intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
        
        if (dif == 0) {
            data = std::move(slot->data);
            slot->sequence.store(pos + Mask + 1, std::memory_order_release);
            dequeuePos_ = pos + 1;
            return true;
        }
        return false; // 队列为空
    }
};

} // namespace hdf
