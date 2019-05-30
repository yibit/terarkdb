//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//

#ifndef ROCKSDB_LITE
#include "memtable/hash_skiplist_rep.h"

#include <atomic>

#include "rocksdb/memtablerep.h"
#include "util/arena.h"
#include "rocksdb/slice.h"
#include "rocksdb/slice_transform.h"
#include "port/port.h"
#include "util/murmurhash.h"
#include "util/string_util.h"
#include "db/memtable.h"
#include "memtable/skiplist.h"

namespace rocksdb {
namespace {

class HashSkipListRep : public MemTableRep {
 public:
  HashSkipListRep(const MemTableRep::KeyComparator& compare,
                  Allocator* allocator, const SliceTransform* transform,
                  size_t bucket_size, int32_t skiplist_height,
                  int32_t skiplist_branching_factor);

  virtual void Insert(KeyHandle handle) override;

  virtual bool Contains(const Slice& internal_key) const override;

  virtual size_t ApproximateMemoryUsage() override;

  virtual void Get(const LookupKey& k, void* callback_args,
                   bool (*callback_func)(void* arg,
                                         const KeyValuePair*)) override;

  virtual ~HashSkipListRep();

  virtual MemTableRep::Iterator* GetIterator(Arena* arena = nullptr) override;

  virtual MemTableRep::Iterator* GetDynamicPrefixIterator(
      Arena* arena = nullptr) override;

 private:
  friend class DynamicIterator;
  typedef SkipList<const char*, const MemTableRep::KeyComparator&> Bucket;

  size_t bucket_size_;

  const int32_t skiplist_height_;
  const int32_t skiplist_branching_factor_;

  // Maps slices (which are transformed user keys) to buckets of keys sharing
  // the same transform.
  std::atomic<Bucket*>* buckets_;

  // The user-supplied transform whose domain is the user keys.
  const SliceTransform* transform_;

  const MemTableRep::KeyComparator& compare_;
  // immutable after construction
  Allocator* const allocator_;

  inline size_t GetHash(const Slice& slice) const {
    return MurmurHash(slice.data(), static_cast<int>(slice.size()), 0) %
           bucket_size_;
  }
  inline Bucket* GetBucket(size_t i) const {
    return buckets_[i].load(std::memory_order_acquire);
  }
  inline Bucket* GetBucket(const Slice& slice) const {
    return GetBucket(GetHash(slice));
  }
  // Get a bucket from buckets_. If the bucket hasn't been initialized yet,
  // initialize it before returning.
  Bucket* GetInitializedBucket(const Slice& transformed);

  class Iterator : public MemTableRep::Iterator {
   public:
    explicit Iterator(Bucket* list, bool own_list = true,
                      Arena* arena = nullptr)
        : list_(list), iter_(list), own_list_(own_list), arena_(arena) {}

    virtual ~Iterator() {
      // if we own the list, we should also delete it
      if (own_list_) {
        assert(list_ != nullptr);
        delete list_;
      }
    }

    // Returns true iff the iterator is positioned at a valid node.
    virtual bool Valid() const override {
      return list_ != nullptr && iter_.Valid();
    }

    // Returns the key at the current position.
    // REQUIRES: Valid()
    virtual const char* key() const override {
      assert(Valid());
      return iter_.key();
    }

    // Advances to the next position.
    // REQUIRES: Valid()
    virtual void Next() override {
      assert(Valid());
      iter_.Next();
    }

    // Advances to the previous position.
    // REQUIRES: Valid()
    virtual void Prev() override {
      assert(Valid());
      iter_.Prev();
    }

    // Advance to the first entry with a key >= target
    virtual void Seek(const Slice& internal_key,
                      const char* memtable_key) override {
      if (list_ != nullptr) {
        const char* encoded_key =
            (memtable_key != nullptr) ?
                memtable_key : EncodeKey(&tmp_, internal_key);
        iter_.Seek(encoded_key);
      }
    }

    // Retreat to the last entry with a key <= target
    virtual void SeekForPrev(const Slice& /*internal_key*/,
                             const char* /*memtable_key*/) override {
      // not supported
      assert(false);
    }

    // Position at the first entry in collection.
    // Final state of iterator is Valid() iff collection is not empty.
    virtual void SeekToFirst() override {
      if (list_ != nullptr) {
        iter_.SeekToFirst();
      }
    }

    // Position at the last entry in collection.
    // Final state of iterator is Valid() iff collection is not empty.
    virtual void SeekToLast() override {
      if (list_ != nullptr) {
        iter_.SeekToLast();
      }
    }
   protected:
    void Reset(Bucket* list) {
      if (own_list_) {
        assert(list_ != nullptr);
        delete list_;
      }
      list_ = list;
      iter_.SetList(list);
      own_list_ = false;
    }
   private:
    // if list_ is nullptr, we should NEVER call any methods on iter_
    // if list_ is nullptr, this Iterator is not Valid()
    Bucket* list_;
    Bucket::Iterator iter_;
    // here we track if we own list_. If we own it, we are also
    // responsible for it's cleaning. This is a poor man's std::shared_ptr
    bool own_list_;
    std::unique_ptr<Arena> arena_;
    std::string tmp_;       // For passing to EncodeKey
  };

  class DynamicIterator : public HashSkipListRep::Iterator {
   public:
    explicit DynamicIterator(const HashSkipListRep& memtable_rep)
      : HashSkipListRep::Iterator(nullptr, false),
        memtable_rep_(memtable_rep) {}

    // Advance to the first entry with a key >= target
    virtual void Seek(const Slice& k, const char* memtable_key) override {
      auto transformed = memtable_rep_.transform_->Transform(ExtractUserKey(k));
      Reset(memtable_rep_.GetBucket(transformed));
      HashSkipListRep::Iterator::Seek(k, memtable_key);
    }

    // Position at the first entry in collection.
    // Final state of iterator is Valid() iff collection is not empty.
    virtual void SeekToFirst() override {
      // Prefix iterator does not support total order.
      // We simply set the iterator to invalid state
      Reset(nullptr);
    }

    // Position at the last entry in collection.
    // Final state of iterator is Valid() iff collection is not empty.
    virtual void SeekToLast() override {
      // Prefix iterator does not support total order.
      // We simply set the iterator to invalid state
      Reset(nullptr);
    }
   private:
    // the underlying memtable
    const HashSkipListRep& memtable_rep_;
  };

  class EmptyIterator : public MemTableRep::Iterator {
    // This is used when there wasn't a bucket. It is cheaper than
    // instantiating an empty bucket over which to iterate.
   public:
    EmptyIterator() { }
    virtual bool Valid() const override { return false; }
    virtual const char* key() const override {
      assert(false);
      return nullptr;
    }
    virtual void Next() override {}
    virtual void Prev() override {}
    virtual void Seek(const Slice& /*internal_key*/,
                      const char* /*memtable_key*/) override {}
    virtual void SeekForPrev(const Slice& /*internal_key*/,
                             const char* /*memtable_key*/) override {}
    virtual void SeekToFirst() override {}
    virtual void SeekToLast() override {}

   private:
  };
};

HashSkipListRep::HashSkipListRep(const MemTableRep::KeyComparator& compare,
                                 Allocator* allocator,
                                 const SliceTransform* transform,
                                 size_t bucket_size, int32_t skiplist_height,
                                 int32_t skiplist_branching_factor)
    : MemTableRep(allocator),
      bucket_size_(bucket_size),
      skiplist_height_(skiplist_height),
      skiplist_branching_factor_(skiplist_branching_factor),
      transform_(transform),
      compare_(compare),
      allocator_(allocator) {
  auto mem = allocator->AllocateAligned(
               sizeof(std::atomic<void*>) * bucket_size);
  buckets_ = new (mem) std::atomic<Bucket*>[bucket_size];

  for (size_t i = 0; i < bucket_size_; ++i) {
    buckets_[i].store(nullptr, std::memory_order_relaxed);
  }
}

HashSkipListRep::~HashSkipListRep() {
}

HashSkipListRep::Bucket* HashSkipListRep::GetInitializedBucket(
    const Slice& transformed) {
  size_t hash = GetHash(transformed);
  auto bucket = GetBucket(hash);
  if (bucket == nullptr) {
    auto addr = allocator_->AllocateAligned(sizeof(Bucket));
    bucket = new (addr) Bucket(compare_, allocator_, skiplist_height_,
                               skiplist_branching_factor_);
    buckets_[hash].store(bucket, std::memory_order_release);
  }
  return bucket;
}

void HashSkipListRep::Insert(KeyHandle handle) {
  auto* key = static_cast<char*>(handle);
  Slice internal_key = GetLengthPrefixedSlice(key);
  assert(!Contains(internal_key));
  auto transformed = transform_->Transform(ExtractUserKey(internal_key));
  auto bucket = GetInitializedBucket(transformed);
  bucket->Insert(key);
}

bool HashSkipListRep::Contains(const Slice& internal_key) const {
  auto transformed = transform_->Transform(ExtractUserKey(internal_key));
  auto bucket = GetBucket(transformed);
  if (bucket == nullptr) {
    return false;
  }
  std::string memtable_key;
  return bucket->Contains(EncodeKey(&memtable_key, internal_key));
}

size_t HashSkipListRep::ApproximateMemoryUsage() {
  return 0;
}

void HashSkipListRep::Get(const LookupKey& k, void* callback_args,
                          bool (*callback_func)(void* arg,
                                                const KeyValuePair*)) {
  auto transformed = transform_->Transform(k.user_key());
  auto bucket = GetBucket(transformed);
  if (bucket != nullptr) {
    EncodedKeyValuePair pair;
    Bucket::Iterator iter(bucket);
    for (iter.Seek(k.memtable_key().data());
         iter.Valid() && callback_func(callback_args, pair.SetKey(iter.key()));
         iter.Next()) {
    }
  }
}

MemTableRep::Iterator* HashSkipListRep::GetIterator(Arena* arena) {
  // allocate a new arena of similar size to the one currently in use
  Arena* new_arena = new Arena(allocator_->BlockSize());
  auto list = new Bucket(compare_, new_arena);
  for (size_t i = 0; i < bucket_size_; ++i) {
    auto bucket = GetBucket(i);
    if (bucket != nullptr) {
      Bucket::Iterator itr(bucket);
      for (itr.SeekToFirst(); itr.Valid(); itr.Next()) {
        list->Insert(itr.key());
      }
    }
  }
  if (arena == nullptr) {
    return new Iterator(list, true, new_arena);
  } else {
    auto mem = arena->AllocateAligned(sizeof(Iterator));
    return new (mem) Iterator(list, true, new_arena);
  }
}

MemTableRep::Iterator* HashSkipListRep::GetDynamicPrefixIterator(Arena* arena) {
  if (arena == nullptr) {
    return new DynamicIterator(*this);
  } else {
    auto mem = arena->AllocateAligned(sizeof(DynamicIterator));
    return new (mem) DynamicIterator(*this);
  }
}

} // anon namespace

MemTableRep* HashSkipListRepFactory::CreateMemTableRep(
    const MemTableRep::KeyComparator& compare, bool /*needs_dup_key_check*/,
    Allocator* allocator, const SliceTransform* transform,
    Logger* /*logger*/) {
  return new HashSkipListRep(compare, allocator, transform, bucket_count_,
                             skiplist_height_, skiplist_branching_factor_);
}

MemTableRepFactory* NewHashSkipListRepFactory(
    size_t bucket_count, int32_t skiplist_height,
    int32_t skiplist_branching_factor) {
  return new HashSkipListRepFactory(bucket_count, skiplist_height,
      skiplist_branching_factor);
}

static MemTableRepFactory* NewHashSkipListRepFactory(
    const std::unordered_map<std::string, std::string>& options, Status* /*s*/) {
  auto f = options.begin();

  size_t bucket_count = 1000000; // default
  f = options.find("bucket_count");
  if (options.end() != f) {
    bucket_count = ParseSizeT(f->second);
  }

  int32_t skiplist_height = 4; // default
  f = options.find("skiplist_height");
  if (options.end() != f) {
    skiplist_height = ParseInt(f->second);
  }

  int32_t skiplist_branching_factor = 4; // default
  f = options.find("skiplist_branching_factor");
  if (options.end() != f) {
    skiplist_branching_factor = ParseInt(f->second);
  }

  return new HashSkipListRepFactory(bucket_count, skiplist_height,
                                    skiplist_branching_factor);
}

ROCKSDB_REGISTER_MEM_TABLE("prefix_hash", HashSkipListRepFactory);

} // namespace rocksdb
#endif  // ROCKSDB_LITE
