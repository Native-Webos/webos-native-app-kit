// SPDX-License-Identifier: GPL-3.0-or-later
//
// Values larger than one key/value entry: save games, a downloaded catalogue, a cache of a few MB. The
// Developer Mode jail has no durable writable directory, so they go into the same store (DB8 on the
// TV) split into chunks.
//
//   <name>.m           manifest: {"gen":G,"size":S,"chunks":C,"crc":X}
//   <name>.<G>.<i>     chunk i of generation G (chunk_bytes each, the last one shorter)
//
// put() writes generation G+1's chunks, then replaces the manifest with a revision check, then deletes
// generation G's chunks. A crash or power cut part-way leaves the previous manifest and its chunks
// whole, so a reader sees the old value or the new one, never a mixture; leftovers are removed by the
// next put() or remove() of that name. Two writers of one name: the second gets Status::Conflict.
// get() checks the size and a CRC-32 of the whole value (Status::Corrupt on a mismatch).
//
// Blocking like the store under it: call from a worker. Each chunk is one DB8 round trip on the TV.
#pragma once

#include "wnk/storage/kv.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace wnk::storage {

uint32_t crc32(std::string_view data);

class BlobStore {
 public:
    // `max_blob_bytes` caps one value (Status::TooLarge above it); chunks are `chunk_bytes`, which must
    // fit one store value (base64 inflates binary by a third in DB8, hence 32 KiB under a 64 KiB limit).
    explicit BlobStore(KeyValueStore &kv, size_t max_blob_bytes = 8u << 20, size_t chunk_bytes = 32u << 10);

    // Names: 1..40 characters of [a-z0-9_-], starting with a letter or digit.
    static bool is_valid_name(std::string_view name);

    Status put(std::string_view name, std::string_view data);
    Status get(std::string_view name, std::string *out);
    Status remove(std::string_view name);
    // The stored size without reading the chunks; nullopt if absent or unreadable.
    std::optional<size_t> size(std::string_view name);

 private:
    struct Manifest {
        uint64_t gen = 0;
        size_t size = 0;
        size_t chunks = 0;
        uint32_t crc = 0;
        uint64_t revision = 0;  // of the manifest entry
    };
    Status read_manifest(std::string_view name, Manifest *m);
    // Removes chunks of `gen` from index `from` until one is missing.
    void remove_chunks(std::string_view name, uint64_t gen, size_t from);
    KeyValueStore &kv_;
    size_t max_blob_;
    size_t chunk_;
};

}  // namespace wnk::storage
