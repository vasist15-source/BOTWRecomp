// Upload pool bookkeeping (Metal renderer, metal_draw.mm; tests: runtime/tools/upload_pool_test.cpp).
//
// Transient per-draw data (indices, large support buffers, texture staging) is written front to back
// into chunks of shared GPU memory. The render thread reserves slices of the current chunk; at each
// commit it hands the chunks written since the previous commit to that commit (take_used), and when the
// committed command buffer completes they are given back (give_back, on a completion thread). A chunk returns to the free list once it is full (no longer the one being
// filled) and no commit that may read it is outstanding:
//   inFlight: commits that wrote it and have not completed;
//   pending:  written since the last commit (the next commit will hold it);
//   filling:  the current chunk.
// Shared (share = true): a chunk serves every command buffer committed while it fills. Not shared (the
// renderer's earlier behaviour): each commit retires the chunk it filled, so every command buffer that
// writes anything takes a chunk of its own.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace gfx::upool {

template <class Buf>
struct Chunk {  // fields under Pool's mutex (buf and length are fixed once made)
    Buf buf{};
    uint64_t length = 0;
    uint32_t inFlight = 0;
    bool filling = false, pending = false;
    bool idle() const { return !inFlight && !filling && !pending; }
};

template <class Buf>
class Pool {
 public:
    using ChunkT = Chunk<Buf>;
    struct Slice {
        ChunkT* chunk;
        uint64_t offset;
    };
    static constexpr uint64_t kAlign = 256;

    // render thread: size bytes in the current chunk, or in another one (from the free list, else make(n),
    // which returns a new buffer of n bytes: chunkSize, or size if that is larger)
    template <class Make>
    Slice reserve(uint64_t size, uint64_t chunkSize, Make&& make) {
        size = (size + kAlign - 1) & ~(kAlign - 1);
        if (!cur_ || pos_ + size > cur_->length) {
            std::lock_guard<std::mutex> lk(m_);
            if (ChunkT* full = cur_) {
                full->filling = false;
                if (full->idle()) free_.push_back(full);  // nothing in flight or since the last commit reads it
            }
            const uint64_t want = std::max(chunkSize, size);
            cur_ = nullptr;
            for (size_t i = 0; i < free_.size(); i++)
                if (free_[i]->length >= want) {
                    cur_ = free_[i];
                    free_.erase(free_.begin() + i);
                    break;
                }
            if (!cur_) {
                cur_ = new ChunkT;
                cur_->buf = make(want);
                cur_->length = want;
                made_.fetch_add(1, std::memory_order_relaxed);
                madeBytes_.fetch_add(want, std::memory_order_relaxed);
            }
            cur_->filling = true;
            cur_->pending = true;
            touched_.push_back(cur_);
            pos_ = 0;
        } else if (touched_.empty() || touched_.back() != cur_) {  // first use since the last commit
            std::lock_guard<std::mutex> lk(m_);
            cur_->pending = true;
            touched_.push_back(cur_);
        }
        Slice s{cur_, pos_};
        pos_ += size;
        return s;
    }

    // render thread, at a commit: appends the chunks the commit holds to out (give them back with
    // give_back when it completes). share = false: the commit also retires the current chunk.
    void take_used(bool share, std::vector<ChunkT*>& out) {
        if (touched_.empty() && (!cur_ || share)) return;
        std::lock_guard<std::mutex> lk(m_);
        for (ChunkT* c : touched_) {
            c->pending = false;
            c->inFlight++;
            out.push_back(c);
        }
        touched_.clear();
        if (cur_ && !share) {
            cur_->filling = false;
            if (cur_->idle()) free_.push_back(cur_);  // not written since the last commit
            cur_ = nullptr;
        }
    }

    // any thread: the commit that held these completed (or nothing that reads them was committed)
    void give_back(const std::vector<ChunkT*>& chunks) {
        std::lock_guard<std::mutex> lk(m_);
        for (ChunkT* c : chunks)
            if (--c->inFlight == 0 && c->idle()) free_.push_back(c);
    }

    uint64_t chunks_made() const { return made_.load(std::memory_order_relaxed); }
    uint64_t bytes_made() const { return madeBytes_.load(std::memory_order_relaxed); }
    size_t free_chunks() {
        std::lock_guard<std::mutex> lk(m_);
        return free_.size();
    }

 private:
    std::mutex m_;
    std::vector<ChunkT*> free_;     // m_
    std::vector<ChunkT*> touched_;  // render thread: chunks written since the last commit
    ChunkT* cur_ = nullptr;         // render thread: the chunk being filled
    uint64_t pos_ = 0;
    std::atomic<uint64_t> made_{0}, madeBytes_{0};
};

}  // namespace gfx::upool
