//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "table/get_context.h"
#include "db/merge_helper.h"
#include "db/read_callback.h"
#include "monitoring/file_read_sample.h"
#include "monitoring/perf_context_imp.h"
#include "monitoring/statistics.h"
#include "rocksdb/env.h"
#include "rocksdb/merge_operator.h"
#include "rocksdb/statistics.h"
#include "util/util.h"

namespace rocksdb {

#ifndef ROCKSDB_LITE

namespace {

template <class T>
static void DeleteEntry(const Slice& /*key*/, void* value) {
  T* typed_value = reinterpret_cast<T*>(value);
  delete typed_value;
}

void AppendVarint64(IterKey* key, uint64_t v) {
  char buf[10];
  auto ptr = EncodeVarint64(buf, v);
  key->TrimAppend(key->Size(), buf, ptr - buf);
}

}

#endif  // ROCKSDB_LITE

GetContext::GetContext(const Comparator* ucmp,
                       const MergeOperator* merge_operator, Logger* logger,
                       Statistics* statistics, GetState init_state,
                       const Slice& user_key, LazySlice* lazy_val,
                       bool* value_found, MergeContext* merge_context,
                       const SeparateHelper* separate_helper,
                       SequenceNumber* _max_covering_tombstone_seq, Env* env,
                       SequenceNumber* seq, ReadCallback* callback,
                       bool trivial)
    : ucmp_(ucmp),
      merge_operator_(merge_operator),
      logger_(logger),
      statistics_(statistics),
      state_(init_state),
      user_key_(user_key),
      lazy_val_(lazy_val),
      value_found_(value_found),
      merge_context_(merge_context),
      separate_helper_(separate_helper),
      max_covering_tombstone_seq_(_max_covering_tombstone_seq),
      env_(env),
      seq_(seq),
      min_seq_type_(0),
      replay_log_callback_(nullptr),
      replay_log_arg_(nullptr),
      callback_(callback),
      trivial_(trivial) {
  if (seq_) {
    *seq_ = kMaxSequenceNumber;
  }
  sample_ = should_sample_file_read();
}

// Called from TableCache::Get and Table::Get when file/block in which
// key may exist are not there in TableCache/BlockCache respectively. In this
// case we can't guarantee that key does not exist and are not permitted to do
// IO to be certain.Set the status=kFound and value_found=false to let the
// caller know that key may exist but is not there in memory
void GetContext::MarkKeyMayExist() {
  state_ = kFound;
  if (value_found_ != nullptr) {
    *value_found_ = false;
  }
}

void GetContext::ReportCounters() {
  if (get_context_stats_.num_cache_hit > 0) {
    RecordTick(statistics_, BLOCK_CACHE_HIT, get_context_stats_.num_cache_hit);
  }
  if (get_context_stats_.num_cache_index_hit > 0) {
    RecordTick(statistics_, BLOCK_CACHE_INDEX_HIT,
               get_context_stats_.num_cache_index_hit);
  }
  if (get_context_stats_.num_cache_data_hit > 0) {
    RecordTick(statistics_, BLOCK_CACHE_DATA_HIT,
               get_context_stats_.num_cache_data_hit);
  }
  if (get_context_stats_.num_cache_filter_hit > 0) {
    RecordTick(statistics_, BLOCK_CACHE_FILTER_HIT,
               get_context_stats_.num_cache_filter_hit);
  }
  if (get_context_stats_.num_cache_index_miss > 0) {
    RecordTick(statistics_, BLOCK_CACHE_INDEX_MISS,
               get_context_stats_.num_cache_index_miss);
  }
  if (get_context_stats_.num_cache_filter_miss > 0) {
    RecordTick(statistics_, BLOCK_CACHE_FILTER_MISS,
               get_context_stats_.num_cache_filter_miss);
  }
  if (get_context_stats_.num_cache_data_miss > 0) {
    RecordTick(statistics_, BLOCK_CACHE_DATA_MISS,
               get_context_stats_.num_cache_data_miss);
  }
  if (get_context_stats_.num_cache_bytes_read > 0) {
    RecordTick(statistics_, BLOCK_CACHE_BYTES_READ,
               get_context_stats_.num_cache_bytes_read);
  }
  if (get_context_stats_.num_cache_miss > 0) {
    RecordTick(statistics_, BLOCK_CACHE_MISS,
               get_context_stats_.num_cache_miss);
  }
  if (get_context_stats_.num_cache_add > 0) {
    RecordTick(statistics_, BLOCK_CACHE_ADD, get_context_stats_.num_cache_add);
  }
  if (get_context_stats_.num_cache_bytes_write > 0) {
    RecordTick(statistics_, BLOCK_CACHE_BYTES_WRITE,
               get_context_stats_.num_cache_bytes_write);
  }
  if (get_context_stats_.num_cache_index_add > 0) {
    RecordTick(statistics_, BLOCK_CACHE_INDEX_ADD,
               get_context_stats_.num_cache_index_add);
  }
  if (get_context_stats_.num_cache_index_bytes_insert > 0) {
    RecordTick(statistics_, BLOCK_CACHE_INDEX_BYTES_INSERT,
               get_context_stats_.num_cache_index_bytes_insert);
  }
  if (get_context_stats_.num_cache_data_add > 0) {
    RecordTick(statistics_, BLOCK_CACHE_DATA_ADD,
               get_context_stats_.num_cache_data_add);
  }
  if (get_context_stats_.num_cache_data_bytes_insert > 0) {
    RecordTick(statistics_, BLOCK_CACHE_DATA_BYTES_INSERT,
               get_context_stats_.num_cache_data_bytes_insert);
  }
  if (get_context_stats_.num_cache_filter_add > 0) {
    RecordTick(statistics_, BLOCK_CACHE_FILTER_ADD,
               get_context_stats_.num_cache_filter_add);
  }
  if (get_context_stats_.num_cache_filter_bytes_insert > 0) {
    RecordTick(statistics_, BLOCK_CACHE_FILTER_BYTES_INSERT,
               get_context_stats_.num_cache_filter_bytes_insert);
  }
}

bool GetContext::SaveValue(const ParsedInternalKey& parsed_key,
                           LazySlice&& value, bool* matched) {
  assert(matched);
  assert((state_ != kMerge && parsed_key.type != kTypeMerge) ||
         merge_context_ != nullptr);
  if (ucmp_->Equal(parsed_key.user_key, user_key_)) {
    uint64_t seq_type =
        PackSequenceAndType(parsed_key.sequence, parsed_key.type);
    if (seq_type < min_seq_type_) {
      // for map sst, this key is masked
      return false;
    }
    *matched = true;
    // If the value is not in the snapshot, skip it
    if (!CheckCallback(parsed_key.sequence)) {
      return true;  // to continue to the next seq
    }

    if (seq_ != nullptr) {
      // Set the sequence number if it is uninitialized
      if (*seq_ == kMaxSequenceNumber) {
        *seq_ = parsed_key.sequence;
      }
    }

    auto type = parsed_key.type;
    // Key matches. Process it
    if ((type == kTypeValue || type == kTypeMerge || type == kTypeValueIndex ||
         type == kTypeMergeIndex) && max_covering_tombstone_seq_ != nullptr &&
        *max_covering_tombstone_seq_ > parsed_key.sequence) {
      type = kTypeRangeDeletion;
      value.reset();
    }
    if (replay_log_callback_) {
      replay_log_callback_(replay_log_arg_, type, value);
    }
    switch (type) {
      case kTypeValueIndex:
        separate_helper_->TransToCombined(user_key_, seq_type, value);
        FALLTHROUGH_INTENDED;
      case kTypeValue:
        assert(state_ == kNotFound || state_ == kMerge);
        if (trivial_) {
          assert(kNotFound == state_);
          assert(lazy_val_ != nullptr);
          state_ = kFound;
          *lazy_val_ = std::move(value);
          return false;
        }
        if (kNotFound == state_) {
          state_ = kFound;
          if (LIKELY(lazy_val_ != nullptr)) {
            value.decode_destructive(*lazy_val_);
          }
        } else if (kMerge == state_) {
          assert(merge_operator_ != nullptr);
          state_ = kFound;
          if (LIKELY(lazy_val_ != nullptr)) {
            Status merge_status = MergeHelper::TimedFullMerge(
                merge_operator_, user_key_, &value,
                merge_context_->GetOperands(), lazy_val_, logger_, statistics_,
                env_);
            if (!merge_status.ok()) {
              state_ = kCorrupt;
            }
            lazy_val_->pin_resource();
          }
        }
        return false;

      case kTypeDeletion:
      case kTypeSingleDeletion:
      case kTypeRangeDeletion:
        // TODO(noetzli): Verify correctness once merge of single-deletes
        // is supported
        assert(state_ == kNotFound || state_ == kMerge);
        if (kNotFound == state_) {
          state_ = kDeleted;
        } else if (kMerge == state_) {
          state_ = kFound;
          if (LIKELY(lazy_val_ != nullptr)) {
            Status merge_status = MergeHelper::TimedFullMerge(
                merge_operator_, user_key_, nullptr,
                merge_context_->GetOperands(), lazy_val_, logger_, statistics_,
                env_);
            if (!merge_status.ok()) {
              state_ = kCorrupt;
            }
            lazy_val_->pin_resource();
          }
        }
        return false;

      case kTypeMergeIndex:
        separate_helper_->TransToCombined(user_key_, seq_type, value);
        FALLTHROUGH_INTENDED;
      case kTypeMerge:
        assert(state_ == kNotFound || state_ == kMerge);
        state_ = kMerge;
        if (trivial_) {
          assert(kNotFound == state_);
          assert(lazy_val_ != nullptr);
          *lazy_val_ = std::move(value);
          return false;
        }
        merge_context_->PushOperand(std::move(value));
        if (merge_operator_ != nullptr &&
            merge_operator_->ShouldMerge(
                merge_context_->GetOperandsDirectionBackward())) {
          state_ = kFound;
          if (LIKELY(lazy_val_ != nullptr)) {
            Status merge_status = MergeHelper::TimedFullMerge(
                merge_operator_, user_key_, nullptr,
                merge_context_->GetOperands(), lazy_val_, logger_, statistics_,
                env_);
            if (!merge_status.ok()) {
              state_ = kCorrupt;
            }
            lazy_val_->pin_resource();
          }
          return false;
        }
        return true;

      default:
        assert(false);
        break;
    }
  }

  // state_ could be Corrupt, merge or notfound
  return false;
}

void GetContext::SetReplayLog(AddReplayLogCallback replay_log_callback,
                              void* replay_log_arg) {
#ifndef ROCKSDB_LITE
  if (replay_log_callback == nullptr && replay_log_callback_ != nullptr &&
      (state_ == kNotFound || state_ == kMerge) &&
      max_covering_tombstone_seq_ != nullptr &&
      *max_covering_tombstone_seq_ != 0) {
    replay_log_callback_(replay_log_arg_, kTypeRangeDeletion, LazySlice());
  }
  replay_log_callback_ = replay_log_callback;
  replay_log_arg_ = replay_log_arg;
#endif  // ROCKSDB_LITE
}

#ifndef ROCKSDB_LITE

bool RowCacheContext::GetFromRowCache(
    const rocksdb::ReadOptions& options, const rocksdb::Slice& key,
    SequenceNumber largest_seqno, IterKey* cache_key, rocksdb::Cache* row_cache,
    const rocksdb::Slice& row_cache_id, uint64_t file_number,
    Statistics* statistics, GetContext* get_context) {
  assert(row_cache != nullptr && !get_context->NeedToReadSequence());

  auto user_key = ExtractUserKey(key);
  // We use the user key as cache key instead of the internal key,
  // otherwise the whole cache would be invalidated every time the
  // sequence key increases. However, to support caching snapshot
  // reads, we append the sequence number only in this case.
  uint64_t seq_no =
      options.snapshot == nullptr
          ? largest_seqno
          : std::min(largest_seqno, GetInternalKeySeqno(key));

  // Compute row cache key.
  cache_key->TrimAppend(cache_key->Size(), row_cache_id.data(),
                        row_cache_id.size());
  AppendVarint64(cache_key, file_number);
  AppendVarint64(cache_key, seq_no);
  cache_key->TrimAppend(cache_key->Size(), user_key.data(), user_key.size());

  auto row_handle = row_cache->Lookup(cache_key->GetUserKey());
  if (!row_handle) {
    RecordTick(statistics, ROW_CACHE_MISS);
    return false;
  }
  // Cleanable routine to release the cache entry
  auto release_cache_entry_func = [](void* cache_to_clean,
                                     void* cache_handle) {
    ((Cache*)cache_to_clean)->Release((Cache::Handle*)cache_handle);
  };
  // If it comes here value is located on the cache.
  // found_row_cache_entry points to the value on cache,
  // and value_pinner has cleanup procedure for the cached entry.
  // After replayGetContextLog() returns, get_context.pinnable_slice_
  // will point to cache entry buffer (or a copy based on that) and
  // cleanup routine under value_pinner will be delegated to
  // get_context.lazy_slice_. Cache entry is released when
  // get_context.lazy_slice_ is reset.
  Slice replay_log =
      *static_cast<const std::string*>(row_cache->Value(row_handle));
  bool first_log = true;
  LazySlice lazy_value;
  while (replay_log.size()) {
    auto type = static_cast<ValueType>(*replay_log.data());
    replay_log.remove_prefix(1);
    Slice value;
    bool ret = GetLengthPrefixedSlice(&replay_log, &value);
    assert(ret);
    (void)ret;

    if (first_log) {
      Cleanable value_pinner;
      value_pinner.RegisterCleanup(release_cache_entry_func, row_cache,
                                   row_handle);
      lazy_value.reset(value, &value_pinner);
      first_log = false;
    } else {
      struct LazySliceControllerImpl : public LazySliceController {
      public:
        void destroy(LazySliceRep* /*rep*/) const override {}
        void pin_resource(LazySlice* slice, LazySliceRep* rep) const override {
          auto c = reinterpret_cast<Cache*>(rep->data[2]);
          auto h = reinterpret_cast<Cache::Handle*>(rep->data[3]);
          c->Ref(h);
          *slice = Slice(reinterpret_cast<const char*>(rep->data[0]),
                         rep->data[1]);
        }
        Status inplace_decode(LazySlice* slice,
                              LazySliceRep* rep) const override {
          pin_resource(slice, rep);
          return Status::OK();
        }
      };
      static LazySliceControllerImpl controller_impl;
      if (value.empty()) {
        lazy_value.reset();
      } else {
        lazy_value.reset(&controller_impl, {
            reinterpret_cast<uint64_t>(value.data()),
            value.size(),
            reinterpret_cast<uint64_t>(row_cache),
            reinterpret_cast<uint64_t>(row_handle),
        });
      }
    }

    bool dont_care __attribute__((__unused__));
    // Since SequenceNumber is not stored and unknown, we will use
    // kMaxSequenceNumber.
    get_context->SaveValue(
        ParsedInternalKey(user_key, kMaxSequenceNumber, type),
        std::move(lazy_value), &dont_care);
  }
  RecordTick(statistics, ROW_CACHE_HIT);
  return true;
}

void RowCacheContext::AddReplayLog(void* arg, rocksdb::ValueType type,
                                   const rocksdb::LazySlice& value) {
  RowCacheContext* context = static_cast<RowCacheContext*>(arg);
  if (context->status.ok()) {
    context->status = value.inplace_decode();
  }
  if (!context->status.ok()) {
    return;
  }
  auto& replay_log = context->buffer;
  if (!replay_log) {
    // Optimization: in the common case of only one operation in the
    // log, we allocate the exact amount of space needed.
    replay_log.reset(new std::string());
    replay_log->reserve(1 + VarintLength(value.size()) + value.size());
  }
  replay_log->push_back(type);
  PutLengthPrefixedSlice(replay_log.get(), value);
}

Status RowCacheContext::AddToCache(const IterKey& cache_key,
                                   rocksdb::Cache* cache) {
  if (status.ok() && buffer) {
    assert(!cache_key.GetUserKey().empty());
    size_t charge = cache_key.Size() + buffer->size() + sizeof(std::string);
    cache->Insert(cache_key.GetUserKey(), buffer.release(), charge,
                  &DeleteEntry<std::string>);
  }
  return status;
}

#endif  // ROCKSDB_LITE

}  // namespace rocksdb
