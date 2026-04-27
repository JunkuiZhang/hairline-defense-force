#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace hdf {

template <typename Key, typename Value> class FastHashmap {
    static constexpr uint16_t NULL_INDEX = 0xFFFF;

    struct Slot {
        Key key;
        Value value;
        uint16_t probe_length{NULL_INDEX};
    };

  public:
    FastHashmap() {}
    FastHashmap(size_t capacity) : capacity(capacity) { data.resize(capacity); }
    ~FastHashmap() {}

    void set_capacity(size_t capacity) noexcept {
        this->capacity = capacity;
        data.resize(capacity);
    }

    void insert(const Key &key, const Value &value) {
        size_t hash_result = std::hash<Key>{}(key);
        size_t index = hash_result % capacity;
        Slot current{key, value, 0};

        while (true) {
            Slot &slot = data[index];
            if (slot.probe_length == NULL_INDEX) {
                slot = current;
                return;
            }
            if (slot.key == current.key) {
                slot.value = current.value;
                return;
            }
            // 罗宾汉原则：劫富济贫
            if (slot.probe_length < current.probe_length) {
                std::swap(slot.key, current.key);
                std::swap(slot.value, current.value);
                std::swap(slot.probe_length, current.probe_length);
            }
            current.probe_length++;
            index = (index + 1) % capacity;
        }
    }

    // 返回指针替代 std::optional<Value&>，避免编译错误并保证零拷贝
    Value *get(const Key &key) {
        size_t hash_result = std::hash<Key>{}(key);
        size_t index = hash_result % capacity;
        uint16_t probe_length = 0;

        while (true) {
            Slot &slot = data[index];
            if (slot.probe_length == NULL_INDEX ||
                slot.probe_length < probe_length) {
                return nullptr;
            }
            if (slot.key == key) {
                return &slot.value;
            }
            probe_length++;
            index = (index + 1) % capacity;
        }
    }

    // 重载 operator[]
    Value &operator[](const Key &key) {
        size_t hash_result = std::hash<Key>{}(key);
        size_t index = hash_result % capacity;
        uint16_t probe_length = 0;

        // 预备一个带有默认构造 Value 的槽位（若发生插入时使用）
        Slot current{key, Value{}, 0};
        bool inserting = false;
        Value *ret_ptr = nullptr;

        while (true) {
            Slot &slot = data[index];

            if (!inserting) {
                // 场景 1：遇到空洞，直接放入新元素并返回
                if (slot.probe_length == NULL_INDEX) {
                    slot = {key, Value{}, probe_length};
                    return slot.value;
                }
                // 场景 2：找到了已存在的 Key，直接返回其引用
                if (slot.key == key) {
                    return slot.value;
                }
                // 场景 3：没找到
                // Key，且当前槽位元素的富裕程度低于我们要插入的新元素
                // 触发罗宾汉原则：把新元素霸占在这个位置，把老元素挤走
                if (slot.probe_length < probe_length) {
                    current.probe_length = probe_length;
                    std::swap(slot, current); // slot 现在是我们新插入的元素
                    ret_ptr = &slot.value;    // 记录下我们要返回的引用地址
                    inserting = true;         // 开启“移动余党”模式
                }
            } else {
                // 场景 4：我们正在为被挤走的老元素寻找新家
                if (slot.probe_length == NULL_INDEX) {
                    slot = current;
                    return *ret_ptr; // 老元素安顿好了，返回我们刚插入的新元素的引用
                }
                if (slot.probe_length < current.probe_length) {
                    std::swap(slot, current); // 继续劫富济贫
                }
            }

            // 推进探测长度和索引
            if (inserting) {
                current.probe_length++;
            } else {
                probe_length++;
            }
            index = (index + 1) % capacity;
        }
    }

    void remove(const Key &key) {
        size_t hash_result = std::hash<Key>{}(key);
        size_t index = hash_result % capacity;
        uint16_t probe_length = 0;

        while (true) {
            Slot &slot = data[index];
            if (slot.probe_length == NULL_INDEX ||
                slot.probe_length < probe_length) {
                return; // 元素不存在
            }
            if (slot.key == key) {
                // 找到元素后，执行 Backward Shift 删除法
                while (true) {
                    size_t next_index = (index + 1) % capacity;
                    Slot &next_slot = data[next_index];

                    // 如果下一个槽位是空的，或者下一个元素原本就属于这个槽位（没有偏移过）
                    // 那么当前的空洞就不需要被后续元素填补了，安全标记为 NULL
                    // 即可
                    if (next_slot.probe_length == NULL_INDEX ||
                        next_slot.probe_length == 0) {
                        data[index].probe_length = NULL_INDEX;
                        return;
                    }

                    // 把下一个元素挪到当前位置，并将其探测长度减 1
                    data[index] = next_slot;
                    data[index].probe_length--;
                    index = next_index;
                }
            }
            probe_length++;
            index = (index + 1) % capacity;
        }
    }

  private:
    size_t capacity;
    std::vector<Slot> data;
};

} // namespace hdf