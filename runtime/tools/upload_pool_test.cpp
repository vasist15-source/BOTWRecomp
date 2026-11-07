// Upload pool bookkeeping (runtime/src/gfx/upload_pool.h): chunks shared by the commits that fill them,
// given back only once full and every commit that may read them has completed; the unshared mode (a chunk
// per commit); and a randomized run that checks no slice handed out overlaps one a pending commit or an
// uncompleted command buffer may still read, in any completion order, with sharing switched on and off.
#include "gfx/upload_pool.h"

#include <cassert>
#include <cstdio>
#include <map>
#include <random>
#include <vector>

using Pool = gfx::upool::Pool<int>;
using Chunk = Pool::ChunkT;

static int g_made = 0;
static int make(uint64_t) { return ++g_made; }

constexpr uint64_t kChunk = 1 << 20;

// a full chunk written after a commit is not freed when that commit completes, only after the next one
static void test_pending_after_commit() {
    Pool p;
    std::vector<Chunk*> c1, c2;
    Pool::Slice a = p.reserve(1000, kChunk, make);
    p.take_used(true, c1);
    assert(c1.size() == 1 && c1[0] == a.chunk);
    p.reserve(kChunk - 2048, kChunk, make);  // the rest of the chunk, written for the next commit
    p.reserve(4096, kChunk, make);           // does not fit: a second chunk, the first is full
    assert(p.chunks_made() == 2);
    p.give_back(c1);
    assert(p.free_chunks() == 0);  // still read by the uncommitted work
    p.take_used(true, c2);
    assert(c2.size() == 2);
    p.give_back(c2);
    assert(p.free_chunks() == 1);  // the full one; the second is still being filled
    std::puts("pending after commit: ok");
}

// shared: many commits of small slices use one chunk; unshared: a chunk per commit while they are in flight
static void test_share_modes() {
    {
        Pool p;
        std::vector<std::vector<Chunk*>> held;
        for (int i = 0; i < 100; i++) {
            p.reserve(512, kChunk, make);
            held.emplace_back();
            p.take_used(true, held.back());
        }
        assert(p.chunks_made() == 1);
        for (auto& h : held) p.give_back(h);
        assert(p.free_chunks() == 0);  // still the current chunk
    }
    {
        Pool p;
        std::vector<std::vector<Chunk*>> held;
        for (int i = 0; i < 10; i++) {
            p.reserve(512, kChunk, make);
            held.emplace_back();
            p.take_used(false, held.back());
        }
        assert(p.chunks_made() == 10);
        for (auto& h : held) p.give_back(h);
        assert(p.free_chunks() == 10);
        p.reserve(512, kChunk, make);  // reuses a free chunk
        assert(p.chunks_made() == 10 && p.free_chunks() == 9);
    }
    {  // a commit with nothing written gives nothing; a slice larger than a chunk gets a chunk of its size
        Pool p;
        std::vector<Chunk*> c;
        p.take_used(true, c);
        assert(c.empty());
        Pool::Slice big = p.reserve(3 * kChunk + 5, kChunk, make);
        assert(big.offset == 0 && big.chunk->length >= 3 * kChunk + 5);
    }
    std::puts("share modes: ok");
}

// randomized: every slice is checked against the slices that pending or uncompleted commits may read
static void test_random(uint32_t seed) {
    std::mt19937 rng(seed);
    Pool p;
    struct Live {
        const Chunk* chunk;
        uint64_t lo, hi;
    };
    std::vector<Live> open;                                 // written since the last commit
    std::map<int, std::pair<std::vector<Chunk*>, std::vector<Live>>> flying;  // commit -> chunks it holds, its slices
    int next = 0;
    bool share = true;
    auto overlaps = [&](const Live& s) {
        auto hit = [&](const Live& o) { return o.chunk == s.chunk && s.lo < o.hi && o.lo < s.hi; };
        for (auto& o : open)
            if (hit(o)) return true;
        for (auto& [id, f] : flying)
            for (auto& o : f.second)
                if (hit(o)) return true;
        return false;
    };
    for (int step = 0; step < 40000; step++) {
        const uint32_t r = rng() % 100;
        if (r < 70) {
            const uint64_t size = rng() % 8 == 0 ? (rng() % (2 * kChunk)) + 1 : (rng() % 20000) + 1;
            Pool::Slice s = p.reserve(size, kChunk, make);
            Live l{s.chunk, s.offset, s.offset + ((size + 255) & ~255ull)};
            assert(l.hi <= s.chunk->length);
            if (overlaps(l)) {
                std::printf("seed %u step %d: slice %llu-%llu of a chunk still read\n", seed, step, (unsigned long long)l.lo,
                            (unsigned long long)l.hi);
                assert(false);
            }
            open.push_back(l);
        } else if (r < 85) {  // commit
            auto& f = flying[next++];
            p.take_used(share, f.first);
            f.second.swap(open);
            open.clear();
        } else if (r < 98) {  // a completion, any outstanding commit (Metal completes in order; this is harder)
            if (flying.empty()) continue;
            auto it = flying.begin();
            std::advance(it, rng() % flying.size());
            p.give_back(it->second.first);
            flying.erase(it);
        } else {
            share = !share;
        }
    }
    // drain: commit, complete everything, retire the current chunk: every chunk is free again (no leak)
    {
        auto& f = flying[next++];
        p.take_used(false, f.first);
    }
    for (auto& [id, f] : flying) p.give_back(f.first);
    flying.clear();
    assert(p.free_chunks() == p.chunks_made());
    std::printf("random %u: ok (%llu chunks made)\n", seed, (unsigned long long)p.chunks_made());
}

int main() {
    test_pending_after_commit();
    test_share_modes();
    for (uint32_t seed : {1u, 2u, 3u, 42u, 1234u}) test_random(seed);
    std::puts("upload_pool_test: all passed");
    return 0;
}
