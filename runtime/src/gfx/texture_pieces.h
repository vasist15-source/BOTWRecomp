// Partial texture updates: a texture's guest memory as pieces, each hashed in chunks (metal_surfaces.mm
// check_texture, upload_surface and check_cpu_overwrite; the layout from the address library:
// texture_pieces_layout.h; test: runtime/tools/texture_pieces_test.cpp).
//
// A piece is one slice of one level when the level's slices are separate ranges of guest memory (slice z
// is the level's z-th slice size: thin tile modes of 2D arrays and cube maps), else a whole level (3D
// textures; thick tile modes interleave 4 slices). A chunk is a piece's bytes in one 64 KiB-aligned block
// of guest memory. A full check hashes every chunk; the texture's content hash is the hash of its chunk
// hashes, and when it changes every piece is uploaded.
//
// A check caused by writes to a texture's armed pages (texture_churn.h: not forced, armed before and after
// it) re-hashes only the chunks with a page written since the last arm and uploads only the pieces with a
// changed chunk. The chunks were hashed after that arm, so a chunk with no page stamped since has been
// write-protected all along (write_watch.h) and is unchanged; which chunks were written is read after the
// new arm, so a write racing the check lands before the hash or faults after the new arm and is seen by
// the next check. BotW streams its 63 MiB terrain array (1024x1024 BC1 x 83, 11 levels) a slice at a time
// while the player moves: each streamed slice's write check hashed and uploaded all 63 MiB.
//
// The chunk hashes are a baseline only when they were taken after the arm the watch holds (Table::stamp,
// set by hashed() after each check's hash; a check that arms without hashing, a rescaled texture or new
// ranges leave it behind). Otherwise, and for first, forced (dirty, our own copies, save states) and
// unarmed (dynamic) checks, the check hashes every chunk: the full path.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "texture_churn.h"
#include "write_watch.h"

namespace texpieces {

constexpr uint32_t kChunk = 64u << 10;

// a level as the upload reads it: its guest range, its slices, and the bytes of one slice when slice z is
// the range's z-th sliceBytes bytes (0: the slices are not separate ranges)
struct Level {
    uint32_t addr = 0, size = 0, slices = 1, sliceBytes = 0;
};

struct Piece {
    uint32_t addr, size;      // guest bytes
    uint32_t level, z0, z1;   // slices [z0, z1) of the level
    uint32_t chunk0, chunks;  // its chunks, in address order
};

struct Chunk {
    uint32_t addr, size;
};

struct Table {
    std::vector<Level> levels;
    std::vector<uint32_t> levelEnd;  // the pieces of level l: from levelEnd[l - 1] (0 for level 0) to levelEnd[l]
    std::vector<Piece> pieces;       // by level, then slice
    std::vector<Chunk> chunks;       // by piece
    std::vector<uint64_t> hashes;    // per chunk, at its last hash
    uint64_t bytes = 0;              // what a full hash reads
    uint64_t stamp = 0;              // the watch stamp the hashes were taken after (0: no baseline)
    bool built() const { return !levels.empty(); }
};

inline void add_piece(Table& t, uint64_t addr, uint64_t size, uint32_t level, uint32_t z0, uint32_t z1) {
    Piece p{(uint32_t)addr, (uint32_t)size, level, z0, z1, (uint32_t)t.chunks.size(), 0};
    for (uint64_t a = addr, end = addr + size; a < end;) {
        const uint64_t next = std::min<uint64_t>(end, (a / kChunk + 1) * kChunk);
        t.chunks.push_back({(uint32_t)a, (uint32_t)(next - a)});
        a = next;
    }
    p.chunks = (uint32_t)t.chunks.size() - p.chunk0;
    t.bytes += size;
    t.pieces.push_back(p);
}

// the pieces and chunks of these levels, without a baseline. A level cut into slices leaves out what
// follows its last slice (alignment padding, which the upload does not read).
inline void build(Table& t, const std::vector<Level>& levels) {
    t = Table{};
    t.levels = levels;
    for (uint32_t l = 0; l < (uint32_t)levels.size(); l++) {
        const Level& L = levels[l];
        const uint32_t slices = std::max(L.slices, 1u);
        if (L.sliceBytes && slices > 1 && (uint64_t)L.sliceBytes * slices <= L.size)
            for (uint32_t z = 0; z < slices; z++) add_piece(t, (uint64_t)L.addr + (uint64_t)z * L.sliceBytes, L.sliceBytes, l, z, z + 1);
        else
            add_piece(t, L.addr, L.size, l, 0, slices);
        t.levelEnd.push_back((uint32_t)t.pieces.size());
    }
    t.hashes.assign(t.chunks.size(), 0);
}

// hashes chunks [first, last) of guest memory at mem (a full hash splits the chunks among threads)
template <class Hash>
inline void hash_chunks(Table& t, const uint8_t* mem, size_t first, size_t last, Hash&& hash) {
    for (size_t i = first; i < last; i++) t.hashes[i] = hash(mem + t.chunks[i].addr, (size_t)t.chunks[i].size);
}

// the texture's content hash: the hash of its chunk hashes
template <class Hash>
inline uint64_t content(const Table& t, Hash&& hash) {
    return hash(t.hashes.data(), t.hashes.size() * sizeof(uint64_t));
}

// May this check (texture_churn.h), whose hash is due, hash only what was written? It was caused by
// writes to pages the last check armed (not forced), it leaves them armed, and the chunk hashes are a
// baseline taken after that arm.
inline bool partial_ok(const Table& t, const texchurn::Check& c) {
    return c.hash && c.written && !c.forced && !c.unarmed && t.built() && t.stamp != 0 && t.stamp == c.since;
}

// Re-hashes the chunks with a page written since `since` (the stamp of the arm the hashes were taken
// after) once the check has armed the ranges again, and appends the pieces with a changed chunk to
// `changed`, in order. Returns the bytes hashed.
template <class Hash>
inline uint64_t rehash_written(Table& t, const uint8_t* mem, uint64_t since, std::vector<uint32_t>& changed, Hash&& hash) {
    uint64_t bytes = 0;
    uint32_t p = 0;
    for (uint32_t l = 0; l < (uint32_t)t.levels.size(); l++) {
        const uint32_t end = t.levelEnd[l];
        if (!wwatch::written_since(t.levels[l].addr, t.levels[l].size, since)) {
            p = end;
            continue;
        }
        for (; p < end; p++) {
            const Piece& pc = t.pieces[p];
            if (pc.chunks > 1 && !wwatch::written_since(pc.addr, pc.size, since)) continue;
            bool diff = false;
            for (uint32_t i = pc.chunk0; i < pc.chunk0 + pc.chunks; i++) {
                const Chunk& c = t.chunks[i];
                if (!wwatch::written_since(c.addr, c.size, since)) continue;
                const uint64_t h = hash(mem + c.addr, (size_t)c.size);
                bytes += c.size;
                if (h != t.hashes[i]) {
                    t.hashes[i] = h;
                    diff = true;
                }
            }
            if (diff) changed.push_back(p);
        }
    }
    return bytes;
}

// after a check's hash, full or partial: the hashes are the next check's baseline while the ranges stay armed
inline void hashed(Table& t, const texchurn::Watch& w) { t.stamp = w.unarmed ? 0 : w.stamp; }

// the changed pieces as runs of consecutive slices of one level: f(level, z0, z1)
template <class F>
inline void for_runs(const Table& t, const std::vector<uint32_t>& changed, F&& f) {
    for (size_t i = 0; i < changed.size();) {
        const Piece& a = t.pieces[changed[i]];
        uint32_t z1 = a.z1;
        size_t j = i + 1;
        while (j < changed.size() && t.pieces[changed[j]].level == a.level && t.pieces[changed[j]].z0 == z1) z1 = t.pieces[changed[j++]].z1;
        f(a.level, a.z0, z1);
        i = j;
    }
}

}  // namespace texpieces
