#pragma once

#include <stdlib.h>
#include <stdint.h>

// Lightweight flat-array list template for generic pointers (pages, groups, outputs, etc).
// Replaces LinkedList<T*> for small, frequently-allocated collections.
// Supports insertion at arbitrary positions.
// Saves ~16 bytes per item (vs 8-byte ListNode + 8-byte malloc overhead for pointer).
template<typename T>
class GenericList {
    T* _items = nullptr;
    uint16_t _count = 0;
    uint16_t _capacity = 0;

    void grow() {
        uint16_t new_cap = (_capacity == 0) ? 4 : (_capacity + 4);
        T* new_items = (T*)realloc(_items, new_cap * sizeof(T));
        if (new_items == nullptr) return; // OOM - item will not be added
        _items = new_items;
        _capacity = new_cap;
    }

public:
    GenericList() = default;
    ~GenericList() { free(_items); }

    GenericList(const GenericList&) = delete;
    GenericList& operator=(const GenericList&) = delete;

    // Append item to end
    void add(T item) {
        if (_count >= _capacity) grow();
        if (_count < _capacity)
            _items[_count++] = item;
    }

    // Insert item at specific position (shifts subsequent items right)
    void add(uint16_t position, T item) {
        if (position > _count) position = _count;
        if (_count >= _capacity) grow();
        if (_count < _capacity) {
            // Shift items right to make room
            for (int i = (int)_count - 1; i >= (int)position; --i) {
                _items[i + 1] = _items[i];
            }
            _items[position] = item;
            ++_count;
        }
    }

    T get(int idx) const {
        if (idx < 0 || idx >= (int)_count) return nullptr;
        return _items[idx];
    }

    uint16_t size() const { return _count; }
    bool empty() const { return _count == 0; }

    void clear() { _count = 0; }

    // Release unused capacity after all items have been added
    void shrink_to_fit() {
        if (_capacity > _count) {
            if (_count == 0) {
                free(_items); _items = nullptr; _capacity = 0;
            } else {
                T* new_items = (T*)realloc(_items, _count * sizeof(T));
                if (new_items != nullptr) { _items = new_items; _capacity = _count; }
            }
        }
    }

    void remove(int idx) {
        if (idx < 0 || idx >= (int)_count) return;
        for (int i = idx; i < (int)_count - 1; ++i) {
            _items[i] = _items[i + 1];
        }
        --_count;
    }

    // Iterator support: begin()/end() return T* so range-based for yields T
    T* begin() { return _items; }
    T* end()   { return _items + _count; }
    const T* begin() const { return _items; }
    const T* end()   const { return _items + _count; }
};

// Backward-compatibility alias for existing code during migration.
template<typename T>
using PageList = GenericList<T>;
