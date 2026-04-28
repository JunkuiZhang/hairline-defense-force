#pragma once

#include <bit>
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
    FastHashmap(size_t capacity) { set_capacity(capacity); }
    ~FastHashmap() {}

    void set_capacity(size_t cap) noexcept {
        capacity = std::bit_ceil(cap); // round up to next power of 2
        mask = capacity - 1;
        data.resize(capacity);
    }

    void insert(const Key &key, const Value &value) {
        size_t hash_result = std::hash<Key>{}(key);
        size_t index = hash_result & mask;
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
            if (slot.probe_length < current.probe_length) {
                std::swap(slot.key, current.key);
                std::swap(slot.value, current.value);
                std::swap(slot.probe_length, current.probe_length);
            }
            current.probe_length++;
            index = (index + 1) & mask;
        }
    }

    Value *get(const Key &key) {
        size_t hash_result = std::hash<Key>{}(key);
        size_t index = hash_result & mask;
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
            index = (index + 1) & mask;
        }
    }

    Value &operator[](const Key &key) {
        size_t hash_result = std::hash<Key>{}(key);
        size_t index = hash_result & mask;
        uint16_t probe_length = 0;

        Slot current{key, Value{}, 0};
        bool inserting = false;
        Value *ret_ptr = nullptr;

        while (true) {
            Slot &slot = data[index];

            if (!inserting) {
                if (slot.probe_length == NULL_INDEX) {
                    slot = {key, Value{}, probe_length};
                    return slot.value;
                }
                if (slot.key == key) {
                    return slot.value;
                }
                if (slot.probe_length < probe_length) {
                    current.probe_length = probe_length;
                    std::swap(slot, current);
                    ret_ptr = &slot.value;
                    inserting = true;
                }
            } else {
                if (slot.probe_length == NULL_INDEX) {
                    slot = current;
                    return *ret_ptr;
                }
                if (slot.probe_length < current.probe_length) {
                    std::swap(slot, current);
                }
            }

            if (inserting) {
                current.probe_length++;
            } else {
                probe_length++;
            }
            index = (index + 1) & mask;
        }
    }

    void remove(const Key &key) {
        size_t hash_result = std::hash<Key>{}(key);
        size_t index = hash_result & mask;
        uint16_t probe_length = 0;

        while (true) {
            Slot &slot = data[index];
            if (slot.probe_length == NULL_INDEX ||
                slot.probe_length < probe_length) {
                return;
            }
            if (slot.key == key) {
                while (true) {
                    size_t next_index = (index + 1) & mask;
                    Slot &next_slot = data[next_index];

                    if (next_slot.probe_length == NULL_INDEX ||
                        next_slot.probe_length == 0) {
                        data[index].probe_length = NULL_INDEX;
                        return;
                    }

                    data[index] = next_slot;
                    data[index].probe_length--;
                    index = next_index;
                }
            }
            probe_length++;
            index = (index + 1) & mask;
        }
    }

    template <typename Fn> void for_each(Fn &&fn) {
        for (size_t i = 0; i < capacity; ++i) {
            if (data[i].probe_length != NULL_INDEX) {
                fn(data[i].key, data[i].value);
            }
        }
    }

  private:
    size_t capacity;
    size_t mask;
    std::vector<Slot> data;
};

} // namespace hdf
