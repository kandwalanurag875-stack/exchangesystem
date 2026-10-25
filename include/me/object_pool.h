// object_pool.h — a pre-allocated free-list pool for fixed-size nodes.
//
// Why this exists (Section 7.2, Section 15): calling new/delete per order would
// dominate latency and add unpredictable tail spikes from the system allocator.
// We allocate memory in big blocks up front and hand out slots from a free
// list. After warm-up, allocate()/deallocate() are O(1) pointer juggles with
// ZERO calls into malloc on the hot path.
//
// It is intentionally NOT thread-safe: only the single matching/writer thread
// ever touches the book and therefore the pool (the single-writer principle,
// Section 7.1). That lets us avoid atomics here entirely.
#ifndef ME_OBJECT_POOL_H
#define ME_OBJECT_POOL_H

#include <cstddef>
#include <memory>
#include <vector>

namespace me {

template <class T>
class ObjectPool {
public:
    explicit ObjectPool(std::size_t initial_capacity = 1u << 16) {
        if (initial_capacity == 0) initial_capacity = 1024;
        block_size_ = initial_capacity;
        grow(block_size_);
    }

    // O(1) after warm-up. Only allocates (off the hot path) if the free list is
    // exhausted — pre-size the pool to your expected peak to avoid that.
    T* allocate() {
        if (free_list_.empty()) {
            grow(block_size_);          // amortised; doubles total capacity
            block_size_ *= 2;
        }
        T* slot = free_list_.back();
        free_list_.pop_back();
        ++live_;
        return slot;
    }

    void deallocate(T* p) {
        if (!p) return;
        free_list_.push_back(p);
        --live_;
    }

    std::size_t live() const { return live_; }      // currently handed out
    std::size_t capacity() const { return capacity_; }

private:
    void grow(std::size_t n) {
        auto block = std::make_unique<T[]>(n);
        T* base = block.get();
        free_list_.reserve(free_list_.size() + n);
        for (std::size_t i = 0; i < n; ++i) free_list_.push_back(base + i);
        blocks_.push_back(std::move(block));
        capacity_ += n;
    }

    std::vector<std::unique_ptr<T[]>> blocks_;   // owns the storage
    std::vector<T*> free_list_;                  // available slots
    std::size_t block_size_ = 0;
    std::size_t capacity_ = 0;
    std::size_t live_ = 0;
};

}  // namespace me

#endif  // ME_OBJECT_POOL_H
