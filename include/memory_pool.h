#pragma once

#include <vector>
#include <memory>
#include <cstdint>
#include <cassert>

namespace hdf {

/**
 * @brief 简单的非线程安全单类型内存池，用于消除热点对象的堆分配开销
 */
template <typename T>
class ObjectPool {
    union Node {
        T data;
        Node* next;
        
        Node() {}
        ~Node() {}
    };

    Node* freeList_{nullptr};
    std::vector<Node*> blocks_;
    size_t blockSize_;

public:
    explicit ObjectPool(size_t blockSize = 1024 * 16) : blockSize_(blockSize) {}

    ~ObjectPool() {
        for (auto block : blocks_) {
            delete[] block;
        }
    }

    template<typename... Args>
    T* allocate(Args&&... args) {
        if (!freeList_) {
            Node* block = new Node[blockSize_];
            for (size_t i = 1; i < blockSize_; ++i) {
                block[i - 1].next = &block[i];
            }
            block[blockSize_ - 1].next = nullptr;
            freeList_ = block;
            blocks_.push_back(block);
        }
        Node* node = freeList_;
        freeList_ = node->next;
        return new (&node->data) T(std::forward<Args>(args)...); // placement new
    }

    void deallocate(T* p) {
        if (!p) return;
        p->~T();
        Node* node = reinterpret_cast<Node*>(p);
        node->next = freeList_;
        freeList_ = node;
    }
};

} // namespace hdf
