/*
 * Copyright 2014 Google Inc. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef KYTHE_CXX_COMMON_INDEXING_KYTHE_OUTPUT_STREAM_H_
#define KYTHE_CXX_COMMON_INDEXING_KYTHE_OUTPUT_STREAM_H_

#include <memory>
#include <vector>
#include <openssl/sha.h>

#include "google/protobuf/io/coded_stream.h"
#include "google/protobuf/io/zero_copy_stream_impl.h"

#include "kythe/proto/storage.pb.h"
#include "llvm/ADT/StringRef.h"

extern "C" {
struct memcached_st;
}

namespace kythe {

/// A collection of references to the components of a VName.
struct VNameRef {
  llvm::StringRef signature;
  llvm::StringRef corpus;
  llvm::StringRef root;
  llvm::StringRef path;
  llvm::StringRef language;
  explicit VNameRef(const proto::VName &vname)
      : signature(vname.signature().data(), vname.signature().size()),
        corpus(vname.corpus().data(), vname.corpus().size()),
        root(vname.root().data(), vname.root().size()),
        path(vname.path().data(), vname.path().size()),
        language(vname.language().data(), vname.language().size()) {}
  VNameRef() {}
  void Expand(proto::VName *vname) const {
    vname->mutable_signature()->assign(signature.data(), signature.size());
    vname->mutable_corpus()->assign(corpus.data(), corpus.size());
    vname->mutable_root()->assign(root.data(), root.size());
    vname->mutable_path()->assign(path.data(), path.size());
    vname->mutable_language()->assign(language.data(), language.size());
  }
};
/// A collection of references to the components of a single Kythe fact.
struct FactRef {
  const VNameRef *source;
  llvm::StringRef fact_name;
  llvm::StringRef fact_value;
  /// Overwrites all of the fields in `entry` that can differ between single
  /// facts.
  void Expand(proto::Entry *entry) const {
    source->Expand(entry->mutable_source());
    entry->mutable_fact_name()->assign(fact_name.data(), fact_name.size());
    entry->mutable_fact_value()->assign(fact_value.data(), fact_value.size());
  }
};
/// A collection of references to the components of a single Kythe edge.
struct EdgeRef {
  const VNameRef *source;
  llvm::StringRef edge_kind;
  const VNameRef *target;
  /// Overwrites all of the fields in `entry` that can differ between edges
  /// without ordinals.
  void Expand(proto::Entry *entry) const {
    source->Expand(entry->mutable_source());
    target->Expand(entry->mutable_target());
    entry->mutable_edge_kind()->assign(edge_kind.data(), edge_kind.size());
  }
};
/// A collection of references to the components of a single Kythe edge with an
/// ordinal.
struct OrdinalEdgeRef {
  const VNameRef *source;
  llvm::StringRef edge_kind;
  const VNameRef *target;
  uint32_t ordinal;
  /// Overwrites all of the fields in `entry` that can differ between edges with
  /// ordinals.
  void Expand(proto::Entry *entry) const {
    char digits[12];  // strlen("4294967295") + 2
    int dot_ordinal_length = ::sprintf(digits, ".%u", ordinal);
    entry->mutable_edge_kind()->clear();
    entry->mutable_edge_kind()->reserve(dot_ordinal_length + edge_kind.size());
    entry->mutable_edge_kind()->append(edge_kind.data(), edge_kind.size());
    entry->mutable_edge_kind()->append(digits, dot_ordinal_length);
    source->Expand(entry->mutable_source());
    target->Expand(entry->mutable_target());
  }
};

/// \brief Keeps track of whether hashes have been seen before.
class HashCache {
 public:
  using Hash = unsigned char[SHA256_DIGEST_LENGTH];
  static constexpr size_t kHashSize = SHA256_DIGEST_LENGTH;
  virtual ~HashCache() {}
  /// \brief Notes that `hash` was seen.
  virtual void RegisterHash(const Hash &hash) {}
  /// \return true if `hash` has been seen before.
  virtual bool SawHash(const Hash &hash) { return false; }
  /// \brief Sets guidelines about the amount of source data per hash.
  /// \param min_size no fewer than this many bytes should be hashed.
  /// \param max_size no more than this many bytes should be hashed.
  void SetSizeLimits(size_t min_size, size_t max_size) {
    min_size_ = min_size;
    max_size_ = max_size;
  }
  size_t min_size() const { return min_size_; }
  size_t max_size() const { return max_size_; }

 private:
  size_t min_size_ = 0;
  size_t max_size_ = 32 * 1024;
};

/// \brief A `HashCache` that uses a memcached server.
class MemcachedHashCache : public HashCache {
 public:
  ~MemcachedHashCache() override;

  /// \brief Use a memcached instance (e.g. "--SERVER=foo:1234")
  bool OpenMemcache(const std::string &spec);

  void RegisterHash(const Hash &hash) override;

  bool SawHash(const Hash &hash) override;

 private:
  ::memcached_st *cache_ = nullptr;
};

// Interface for receiving Kythe data.
class KytheOutputStream {
 public:
  virtual void Emit(const FactRef &fact) = 0;
  virtual void Emit(const EdgeRef &edge) = 0;
  virtual void Emit(const OrdinalEdgeRef &edge) = 0;
  /// Add a buffer to the buffer stack to group facts, edges, and buffers
  /// together.
  virtual void PushBuffer() {}
  /// Pop the last buffer from the buffer stack.
  virtual void PopBuffer() {}
  /// \brief Use a given `HashCache` to deduplicate buffers.
  virtual void UseHashCache(HashCache *cache) {}
  virtual ~KytheOutputStream() {}
};

/// \brief Manages a stack of size-bounded buffers.
class BufferStack {
 public:
  /// \brief Hashes the buffer at the top of the stack, returning the result
  /// in `hash`.
  void HashTop(HashCache::Hash *hash) const {
    assert(buffers_ != nullptr);
    ::SHA256_CTX sha;
    ::SHA256_Init(&sha);
    for (Buffer *joined = buffers_; joined; joined = joined->joined) {
      ::SHA256_Update(&sha, joined->slab.data(), joined->slab.size());
    }
    ::SHA256_Final(reinterpret_cast<unsigned char *>(hash), &sha);
  }
  /// \brief Copies the buffer at the top of the stack to some `stream`.
  void CopyTopToStream(
      google::protobuf::io::ZeroCopyOutputStream *stream) const {
    for (Buffer *joined = buffers_; joined; joined = joined->joined) {
      void *proto_data;
      int proto_size;
      size_t write_at = 0;
      while (write_at < joined->slab.size()) {
        proto_size = std::min(static_cast<size_t>(INT_MAX),
                              joined->slab.size() - write_at);
        if (!stream->Next(&proto_data, &proto_size)) {
          assert(0 && "bad stream");
        }
        size_t to_copy = std::min(static_cast<size_t>(proto_size),
                                  joined->slab.size() - write_at);
        memcpy(proto_data, joined->slab.data() + write_at, to_copy);
        if (proto_size > to_copy) {
          stream->BackUp(proto_size - to_copy);
        }
        write_at += to_copy;
      }
    }
  }
  /// \brief Allocates space for writing data to the buffer on the top of
  /// the stack.
  /// \return A pointer to `bytes` bytes of storage.
  unsigned char *WriteToTop(size_t bytes) {
    assert(buffers_);
    size_t insertion_point = buffers_->slab.size();
    buffers_->slab.resize(insertion_point + bytes);
    unsigned char *buffer = &buffers_->slab[insertion_point];
    buffers_->joined_size += bytes;
    return buffer;
  }
  /// \brief Pushes a new buffer to the stack.
  /// \param expected_size An estimate of the buffer's maximum size.
  void Push(size_t expected_size) {
    Buffer *buffer = free_buffers_;
    if (buffer) {
      free_buffers_ = buffer->previous;
    } else {
      buffer = new Buffer();
      buffer->slab.reserve(expected_size);
    }
    buffer->joined = nullptr;
    buffer->slab.clear();
    buffer->joined_size = 0;
    buffer->previous = buffers_;
    buffers_ = buffer;
  }
  /// \brief Returns the size of the buffer on the top of the stack.
  size_t top_size() const {
    assert(buffers_);
    return buffers_->joined_size;
  }
  /// \brief Pops the buffer from the top of the stack.
  void Pop() {
    assert(buffers_);
    Buffer *joined = buffers_->joined;
    while (joined) {
      joined->previous = free_buffers_;
      free_buffers_ = joined;
      joined = joined->joined;
    }
    Buffer *to_free = buffers_;
    buffers_ = to_free->previous;
    to_free->previous = free_buffers_;
    free_buffers_ = to_free;
  }
  /// \brief Merge the buffer at the top of the stack with the one below it.
  ///
  /// If the buffer at the top of the stack is smaller than `min_size`,
  /// there is a buffer underneath it, and merging the buffer on top with the
  /// one below would not result in a buffer longer or as long as `max_size`,
  /// performs the merge and returns true. Otherwise does nothing and returns
  /// false.
  ///
  /// No guarantees are made about ordering except that content inside a buffer
  /// will never be mangled.
  bool MergeDownIfTooSmall(size_t min_size, size_t max_size) {
    if (!buffers_ || !buffers_->previous) {
      return false;
    }
    if (buffers_->joined_size >= min_size ||
        buffers_->previous->joined_size + buffers_->joined_size >= max_size) {
      return false;
    }
    Buffer *to_merge = buffers_;
    Buffer *merge_into = buffers_->previous;
    Buffer *merge_into_join_tail = merge_into;
    while (merge_into_join_tail->joined) {
      merge_into_join_tail = merge_into_join_tail->joined;
    }
    merge_into_join_tail->joined = to_merge;
    merge_into->joined_size += to_merge->joined_size;
    buffers_ = merge_into;
    return true;
  }
  bool empty() const { return buffers_ == nullptr; }
  ~BufferStack() {
    while (!empty()) {
      Pop();
    }
    while (free_buffers_) {
      Buffer *previous = free_buffers_->previous;
      delete free_buffers_;
      free_buffers_ = previous;
    }
  }

 private:
  struct Buffer {
    /// Used to allocate storage for messages.
    std::vector<unsigned char> slab;
    /// `size` plus the `size` of all joined buffers.
    size_t joined_size;
    /// The previous buffer on the stack or the freelist.
    Buffer *previous;
    /// A link to the next buffer that was merged with this one.
    Buffer *joined;
  };
  /// The stack of open buffers.
  Buffer *buffers_ = nullptr;
  /// Inactive buffers ready for allocation.
  Buffer *free_buffers_ = nullptr;
};

// A `KytheOutputStream` that records `Entry` instances to a
// `FileOutputStream`.
class FileOutputStream : public KytheOutputStream {
 public:
  explicit FileOutputStream(google::protobuf::io::FileOutputStream *stream)
      : stream_(stream) {
    edge_entry_.set_fact_name("/");
    UseHashCache(&default_cache_);
  }

  /// \brief Dump stats to standard out on destruction?
  void set_show_stats(bool value) { show_stats_ = value; }
  void set_flush_after_each_entry(bool value) {
    flush_after_each_entry_ = value;
  }
  void Emit(const FactRef &fact) override {
    fact.Expand(&fact_entry_);
    EnqueueEntry(fact_entry_);
  }
  void Emit(const EdgeRef &edge) override {
    edge.Expand(&edge_entry_);
    EnqueueEntry(edge_entry_);
  }
  void Emit(const OrdinalEdgeRef &edge) override {
    edge.Expand(&edge_entry_);
    EnqueueEntry(edge_entry_);
  }
  void UseHashCache(HashCache *cache) override {
    cache_ = cache;
    min_size_ = cache_->min_size();
    max_size_ = cache_->max_size();
  }
  ~FileOutputStream() override;
  void PushBuffer() override;
  void PopBuffer() override;

  /// \brief Statistics about delimited deduplication.
  struct Stats {
    /// How many buffers we've emitted.
    size_t buffers_retired_ = 0;
    /// How many buffers we've split.
    size_t buffers_split_ = 0;
    /// How many buffers we've merged together.
    size_t buffers_merged_ = 0;
    /// How many buffers we didn't emit because their hashes matched.
    size_t hashes_matched_ = 0;
    /// How many bytes in total we've seen (whether or not they were emitted).
    size_t total_bytes_ = 0;
    /// \brief Return a summary of these statistics as a string.
    std::string ToString() const;
  } stats_;

 private:
  /// The output stream to write on.
  google::protobuf::io::FileOutputStream *stream_;
  /// A prototypical Kythe fact, used only to build other Kythe facts.
  proto::Entry fact_entry_;
  /// A prototypical Kythe edge, used only to build same.
  proto::Entry edge_entry_;
  /// Buffers we're holding back for deduplication.
  BufferStack buffers_;

  /// Emits all data from the top buffer (if the hash cache says it's relevant).
  void EmitAndReleaseTopBuffer();
  /// Emits an entry or adds it to a buffer (if the stack is nonempty).
  void EnqueueEntry(const proto::Entry &entry);
  /// The minimum size a buffer must be to get emitted.
  size_t min_size_ = 0;
  /// The maximum size a buffer can reach before it's split.
  size_t max_size_ = 32 * 1024;
  /// Whether we should flush the output stream after each entry
  /// (when the buffer stack is empty).
  bool flush_after_each_entry_;
  /// The active hash cache;
  HashCache *cache_;
  /// The default hash cache.
  HashCache default_cache_;
  /// Whether we should dump stats to standard out on destruction.
  bool show_stats_ = false;
};

}  // namespace kythe

#endif  // KYTHE_CXX_COMMON_INDEXING_KYTHE_OUTPUT_STREAM_H_
