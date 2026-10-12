// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/storage/blob.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>

namespace wnk::storage {
namespace {

std::string manifest_key(std::string_view name) { return std::string(name) + ".m"; }

std::string chunk_key(std::string_view name, uint64_t gen, size_t i) {
    return std::string(name) + "." + std::to_string(gen) + "." + std::to_string(i);
}

}  // namespace

uint32_t crc32(std::string_view data) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : data) c = table[(c ^ b) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

BlobStore::BlobStore(KeyValueStore &kv, size_t max_blob_bytes, size_t chunk_bytes)
    : kv_(kv), max_blob_(max_blob_bytes), chunk_(std::max<size_t>(1, std::min(chunk_bytes, kv.max_value_bytes()))) {}

bool BlobStore::is_valid_name(std::string_view name) {
    if (name.empty() || name.size() > 40 || std::isalnum(static_cast<unsigned char>(name[0])) == 0) return false;
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

Status BlobStore::read_manifest(std::string_view name, Manifest *m) {
    Entry e;
    const Status s = kv_.get(manifest_key(name), &e);
    if (s != Status::Ok) return s;
    const auto j = nlohmann::json::parse(e.value, nullptr, false);
    auto unsigned_field = [&j](const char *k) { return j.is_object() && j.contains(k) && j[k].is_number_unsigned(); };
    if (!unsigned_field("gen") || !unsigned_field("size") || !unsigned_field("chunks") || !unsigned_field("crc")) {
        return Status::Corrupt;
    }
    m->gen = j["gen"].get<uint64_t>();
    m->size = j["size"].get<size_t>();
    m->chunks = j["chunks"].get<size_t>();
    m->crc = j["crc"].get<uint32_t>();
    m->revision = e.revision;
    return Status::Ok;
}

void BlobStore::remove_chunks(std::string_view name, uint64_t gen, size_t from) {
    for (size_t i = from; i < 1000000; ++i) {
        if (kv_.remove(chunk_key(name, gen, i)) != Status::Ok) break;
    }
}

Status BlobStore::put(std::string_view name, std::string_view data) {
    if (!is_valid_name(name)) return Status::InvalidKey;
    if (data.size() > max_blob_) return Status::TooLarge;
    Manifest old;
    const Status ms = read_manifest(name, &old);
    if (ms != Status::Ok && ms != Status::NotFound && ms != Status::Corrupt) return ms;
    const bool had = ms == Status::Ok;
    const uint64_t gen = had ? old.gen + 1 : 1;
    const size_t chunks = (data.size() + chunk_ - 1) / chunk_;
    for (size_t i = 0; i < chunks; ++i) {
        const PutResult r = kv_.put(chunk_key(name, gen, i), data.substr(i * chunk_, chunk_));
        if (r.status != Status::Ok) {
            remove_chunks(name, gen, 0);
            return r.status;
        }
    }
    remove_chunks(name, gen, chunks);  // leftovers of an earlier, interrupted put of this generation
    const nlohmann::json manifest = {{"gen", gen}, {"size", data.size()}, {"chunks", chunks}, {"crc", crc32(data)}};
    const PutResult r = kv_.put(manifest_key(name), manifest.dump(), had ? old.revision : (ms == Status::Corrupt ? kAnyRevision : kMustNotExist));
    if (r.status != Status::Ok) {
        remove_chunks(name, gen, 0);  // the manifest still names the old generation
        return r.status;
    }
    if (had) remove_chunks(name, old.gen, 0);
    return Status::Ok;
}

Status BlobStore::get(std::string_view name, std::string *out) {
    if (!is_valid_name(name)) return Status::InvalidKey;
    Manifest m;
    const Status ms = read_manifest(name, &m);
    if (ms != Status::Ok) return ms;
    std::string data;
    data.reserve(m.size);
    for (size_t i = 0; i < m.chunks; ++i) {
        Entry e;
        const Status s = kv_.get(chunk_key(name, m.gen, i), &e);
        if (s == Status::NotFound) return Status::Corrupt;
        if (s != Status::Ok) return s;
        data += e.value;
    }
    if (data.size() != m.size || crc32(data) != m.crc) return Status::Corrupt;
    *out = std::move(data);
    return Status::Ok;
}

Status BlobStore::remove(std::string_view name) {
    if (!is_valid_name(name)) return Status::InvalidKey;
    Manifest m;
    const Status ms = read_manifest(name, &m);
    if (ms == Status::NotFound) return Status::NotFound;
    const Status rs = kv_.remove(manifest_key(name));
    if (rs != Status::Ok && rs != Status::NotFound) return rs;
    if (ms == Status::Ok) {
        remove_chunks(name, m.gen, 0);
        remove_chunks(name, m.gen + 1, 0);  // an interrupted put's
    }
    return Status::Ok;
}

std::optional<size_t> BlobStore::size(std::string_view name) {
    Manifest m;
    if (!is_valid_name(name) || read_manifest(name, &m) != Status::Ok) return std::nullopt;
    return m.size;
}

}  // namespace wnk::storage
