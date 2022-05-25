// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/db_impl.h"

#include <stdint.h>
#include <stdio.h>

#include <algorithm>
#include <atomic>
#include <set>
#include <string>
#include <vector>

#include "db/builder.h"
#include "db/db_iter.h"
#include "db/dbformat.h"
#include "db/filename.h"
#include "db/log_reader.h"
#include "db/log_writer.h"
#include "db/memtable.h"
#include "db/table_cache.h"
#include "db/version_set.h"
#include "db/write_batch_internal.h"
#include "leveldb/db.h"
#include "leveldb/env.h"
#include "leveldb/status.h"
#include "leveldb/table.h"
#include "leveldb/table_builder.h"
#include "port/port.h"
#include "table/block.h"
#include "table/merger.h"
#include "table/two_level_iterator.h"
#include "util/coding.h"
#include "util/logging.h"
#include "util/mutexlock.h"

namespace leveldb {

const int kNumNonTableCacheFiles = 10;

// Information kept for every waiting writer
struct DBImpl::Writer {
  explicit Writer(port::Mutex* mu)
      : batch(nullptr), sync(false), done(false), cv(mu) {}

  Status status;
  WriteBatch* batch;
  bool sync;
  bool done;
  port::CondVar cv;
};

struct DBImpl::CompactionState {
  // Files produced by compaction
  struct Output {
    uint64_t number;
    uint64_t file_size;
    InternalKey smallest, largest;
  };

  Output* current_output() { return &outputs[outputs.size() - 1]; }

  explicit CompactionState(Compaction* c)
      : compaction(c),
        smallest_snapshot(0),
        outfile(nullptr),
        builder(nullptr),
        total_bytes(0) {}

  Compaction* const compaction;

  // 小于smallest_snapshot的记录可以删除。
  // Sequence numbers < smallest_snapshot are not significant since we
  // will never have to service a snapshot below smallest_snapshot.
  // Therefore if we have seen a sequence number S <= smallest_snapshot,
  // we can drop all entries for the same key with sequence numbers < S.
  SequenceNumber smallest_snapshot;

  std::vector<Output> outputs;

  // State kept for output being generated
  WritableFile* outfile;
  TableBuilder* builder;

  uint64_t total_bytes;
};

// Fix user-supplied options to be reasonable
template <class T, class V>
static void ClipToRange(T* ptr, V minvalue, V maxvalue) {
  if (static_cast<V>(*ptr) > maxvalue) *ptr = maxvalue;
  if (static_cast<V>(*ptr) < minvalue) *ptr = minvalue;
}
Options SanitizeOptions(const std::string& dbname,
                        const InternalKeyComparator* icmp,
                        const InternalFilterPolicy* ipolicy,
                        const Options& src) {
  Options result = src;
  result.comparator = icmp;
  result.filter_policy = (src.filter_policy != nullptr) ? ipolicy : nullptr;
  ClipToRange(&result.max_open_files, 64 + kNumNonTableCacheFiles, 50000);
  ClipToRange(&result.write_buffer_size, 64 << 10, 1 << 30);
  ClipToRange(&result.max_file_size, 1 << 20, 1 << 30);
  ClipToRange(&result.block_size, 1 << 10, 4 << 20);
  if (result.info_log == nullptr) {
    // Open a log file in the same directory as the db
    src.env->CreateDir(dbname);  // In case it does not exist
    src.env->RenameFile(InfoLogFileName(dbname), OldInfoLogFileName(dbname));
    Status s = src.env->NewLogger(InfoLogFileName(dbname), &result.info_log);
    if (!s.ok()) {
      // No place suitable for logging
      result.info_log = nullptr;
    }
  }
  if (result.block_cache == nullptr) {
    result.block_cache = NewLRUCache(8 << 20);
  }
  return result;
}

static int TableCacheSize(const Options& sanitized_options) {
  // Reserve ten files or so for other uses and give the rest to TableCache.
  return sanitized_options.max_open_files - kNumNonTableCacheFiles;
}

DBImpl::DBImpl(const Options& raw_options, const std::string& dbname)
    : env_(raw_options.env),
      internal_comparator_(raw_options.comparator),
      internal_filter_policy_(raw_options.filter_policy),
      options_(SanitizeOptions(dbname, &internal_comparator_,
                               &internal_filter_policy_, raw_options)),
      owns_info_log_(options_.info_log != raw_options.info_log),
      owns_cache_(options_.block_cache != raw_options.block_cache),
      dbname_(dbname),
      table_cache_(new TableCache(dbname_, options_, TableCacheSize(options_))),
      db_lock_(nullptr),
      shutting_down_(false),
      background_work_finished_signal_(&mutex_),
      mem_(nullptr),
      mem_hot_(nullptr),
      mem_level0_(nullptr),
      mem_level1_(nullptr),
      mem_level2_(nullptr),
      imm_level2_(nullptr),
      imm_(nullptr),
      has_imm_(false),
      logfile_(nullptr),
      logfile_number_(0),
      log_(nullptr),
      log_hot_(nullptr),
      seed_(0),
      tmp_batch_(new WriteBatch),
      background_compaction_scheduled_(false),
      manual_compaction_(nullptr),
      versions_(new VersionSet(dbname_, &options_, table_cache_,
                               &internal_comparator_)) {}

DBImpl::~DBImpl() {
  // Wait for background work to finish.
  mutex_.Lock();
  shutting_down_.store(true, std::memory_order_release);
  while (background_compaction_scheduled_) {
    background_work_finished_signal_.Wait();
  }
  mutex_.Unlock();

  if (db_lock_ != nullptr) {
    env_->UnlockFile(db_lock_);
  }

  delete versions_;
  if (mem_ != nullptr) mem_->Unref();
  if (imm_ != nullptr) imm_->Unref();
  delete tmp_batch_;
  delete log_;
  delete log_hot_;
  delete logfile_;
  delete table_cache_;

  if (owns_info_log_) {
    delete options_.info_log;
  }
  if (owns_cache_) {
    delete options_.block_cache;
  }

  // 调试代码
  //os1.close();
  //os2.close();
  //os3.close();
  // os4.close();
  //os5.close();
 // os6.close();
 // os7.close();
}

Status DBImpl::NewDB() {
  VersionEdit new_db;
  new_db.SetComparatorName(user_comparator()->Name());
  new_db.SetLogNumber(0);
  new_db.SetNextFile(2);
  new_db.SetLastSequence(0);

  const std::string manifest = DescriptorFileName(dbname_, 1);
  WritableFile* file;
  Status s = env_->NewWritableFile(manifest, &file);
  if (!s.ok()) {
    return s;
  }
  {
    log::Writer log(file);
    std::string record;
    new_db.EncodeTo(&record);
    s = log.AddRecord(record);
    if (s.ok()) {
      s = file->Close();
    }
  }
  delete file;
  if (s.ok()) {
    // Make "CURRENT" file that points to the new manifest file.
    s = SetCurrentFile(env_, dbname_, 1);
  } else {
    env_->DeleteFile(manifest);
  }
  return s;
}

void DBImpl::MaybeIgnoreError(Status* s) const {
  if (s->ok() || options_.paranoid_checks) {
    // No change needed
  } else {
    Log(options_.info_log, "Ignoring error %s", s->ToString().c_str());
    *s = Status::OK();
  }
}

void DBImpl::DeleteObsoleteFiles() {
  mutex_.AssertHeld();

  if (!bg_error_.ok()) {
    // After a background error, we don't know whether a new version may
    // or may not have been committed, so we cannot safely garbage collect.
    return;
  }

  const uint64_t log_number = versions_->LogNumber();
  const uint64_t prev_log_number = versions_->PrevLogNumber();
  const uint64_t manifest_file_number = versions_->ManifestFileNumber();

  // Make a set of all of the live files
  std::set<uint64_t> live = pending_outputs_;
  versions_->AddLiveFiles(&live);

  std::vector<std::string> filenames;
  env_->GetChildren(dbname_, &filenames);  // Ignoring errors on purpose

  // Unlock while deleting obsolete files
  mutex_.Unlock();
  uint64_t number;
  FileType type;
  for (size_t i = 0; i < filenames.size(); i++) {
    if (ParseFileName(filenames[i], &number, &type)) {
      bool keep = true;
      switch (type) {
        case kLogFile:
          keep = ((number >= log_number) || (number == prev_log_number));
          break;
        case kDescriptorFile:
          // Keep my manifest file, and any newer incarnations'
          // (in case there is a race that allows other incarnations)
          keep = (number >= manifest_file_number);
          break;
        case kTableFile:
          keep = (live.find(number) != live.end());
          break;
        case kTempFile:
          // Any temp files that are currently being written to must
          // be recorded in pending_outputs_, which is inserted into "live"
          keep = (live.find(number) != live.end());
          break;
        case kCurrentFile:
        case kDBLockFile:
        case kInfoLogFile:
          keep = true;
          break;
      }

      if (!keep) {
        if (type == kTableFile) {
          table_cache_->Evict(number);
        }
        Log(options_.info_log, "Delete type=%d #%lld\n", static_cast<int>(type),
            static_cast<unsigned long long>(number));
        env_->DeleteFile(dbname_ + "/" + filenames[i]);
      }
    }
  }
  mutex_.Lock();
}

Status DBImpl::Recover(VersionEdit* edit, bool* save_manifest) {
  mutex_.AssertHeld();

  // Ignore error from CreateDir since the creation of the DB is
  // committed only when the descriptor is created, and this directory
  // may already exist from a previous failed creation attempt.
  env_->CreateDir(dbname_);
  assert(db_lock_ == nullptr);
  Status s = env_->LockFile(LockFileName(dbname_), &db_lock_);
  if (!s.ok()) {
    return s;
  }

  if (!env_->FileExists(CurrentFileName(dbname_))) {
    if (options_.create_if_missing) {
      s = NewDB();
      if (!s.ok()) {
        return s;
      }
    } else {
      return Status::InvalidArgument(
          dbname_, "does not exist (create_if_missing is false)");
    }
  } else {
    if (options_.error_if_exists) {
      return Status::InvalidArgument(dbname_,
                                     "exists (error_if_exists is true)");
    }
  }

  s = versions_->Recover(save_manifest);
  if (!s.ok()) {
    return s;
  }
  SequenceNumber max_sequence(0);

  // Recover from all newer log files than the ones named in the
  // descriptor (new log files may have been added by the previous
  // incarnation without registering them in the descriptor).
  //
  // Note that PrevLogNumber() is no longer used, but we pay
  // attention to it in case we are recovering a database
  // produced by an older version of leveldb.
  const uint64_t min_log = versions_->LogNumber();
  const uint64_t prev_log = versions_->PrevLogNumber();
  std::vector<std::string> filenames;
  s = env_->GetChildren(dbname_, &filenames);
  if (!s.ok()) {
    return s;
  }
  std::set<uint64_t> expected;
  versions_->AddLiveFiles(&expected);
  uint64_t number;
  FileType type;
  std::vector<uint64_t> logs;
  for (size_t i = 0; i < filenames.size(); i++) {
    if (ParseFileName(filenames[i], &number, &type)) {
      expected.erase(number);
      if (type == kLogFile && ((number >= min_log) || (number == prev_log)))
        logs.push_back(number);
    }
  }
  if (!expected.empty()) {
    char buf[50];
    snprintf(buf, sizeof(buf), "%d missing files; e.g.",
             static_cast<int>(expected.size()));
    return Status::Corruption(buf, TableFileName(dbname_, *(expected.begin())));
  }

  // Recover in the order in which the logs were generated
  std::sort(logs.begin(), logs.end());
  for (size_t i = 0; i < logs.size(); i++) {
    s = RecoverLogFile(logs[i], (i == logs.size() - 1), save_manifest, edit,
                       &max_sequence);
    if (!s.ok()) {
      return s;
    }

    // The previous incarnation may not have written any MANIFEST
    // records after allocating this log number.  So we manually
    // update the file number allocation counter in VersionSet.
    versions_->MarkFileNumberUsed(logs[i]);
  }

  if (versions_->LastSequence() < max_sequence) {
    versions_->SetLastSequence(max_sequence);
  }

  return Status::OK();
}

Status DBImpl::RecoverLogFile(uint64_t log_number, bool last_log,
                              bool* save_manifest, VersionEdit* edit,
                              SequenceNumber* max_sequence) {
  struct LogReporter : public log::Reader::Reporter {
    Env* env;
    Logger* info_log;
    const char* fname;
    Status* status;  // null if options_.paranoid_checks==false
    void Corruption(size_t bytes, const Status& s) override {
      Log(info_log, "%s%s: dropping %d bytes; %s",
          (this->status == nullptr ? "(ignoring error) " : ""), fname,
          static_cast<int>(bytes), s.ToString().c_str());
      if (this->status != nullptr && this->status->ok()) *this->status = s;
    }
  };

  mutex_.AssertHeld();

  // Open the log file
  std::string fname = LogFileName(dbname_, log_number);
  SequentialFile* file;
  Status status = env_->NewSequentialFile(fname, &file);
  if (!status.ok()) {
    MaybeIgnoreError(&status);
    return status;
  }

  // Create the log reader.
  LogReporter reporter;
  reporter.env = env_;
  reporter.info_log = options_.info_log;
  reporter.fname = fname.c_str();
  reporter.status = (options_.paranoid_checks ? &status : nullptr);
  // We intentionally make log::Reader do checksumming even if
  // paranoid_checks==false so that corruptions cause entire commits
  // to be skipped instead of propagating bad information (like overly
  // large sequence numbers).
  log::Reader reader(file, &reporter, true /*checksum*/, 0 /*initial_offset*/);
  Log(options_.info_log, "Recovering log #%llu",
      (unsigned long long)log_number);

  // Read all the records and add to a memtable
  std::string scratch;
  Slice record;
  WriteBatch batch;
  int compactions = 0;
  MemTable* mem = nullptr;
  while (reader.ReadRecord(&record, &scratch) && status.ok()) {
    if (record.size() < 12) {
      reporter.Corruption(record.size(),
                          Status::Corruption("log record too small"));
      continue;
    }
    WriteBatchInternal::SetContents(&batch, record);

    if (mem == nullptr) {
      mem = new MemTable(internal_comparator_);
      mem->Ref();
    }
    status = WriteBatchInternal::InsertInto(&batch, mem);
    MaybeIgnoreError(&status);
    if (!status.ok()) {
      break;
    }
    const SequenceNumber last_seq = WriteBatchInternal::Sequence(&batch) +
                                    WriteBatchInternal::Count(&batch) - 1;
    if (last_seq > *max_sequence) {
      *max_sequence = last_seq;
    }

    if (mem->ApproximateMemoryUsage() > options_.write_buffer_size) {
      compactions++;
      *save_manifest = true;
      status = WriteLevel0Table(mem, edit, nullptr);
      mem->Unref();
      mem = nullptr;
      if (!status.ok()) {
        // Reflect errors immediately so that conditions like full
        // file-systems cause the DB::Open() to fail.
        break;
      }
    }
  }

  delete file;

  // See if we should keep reusing the last log file.
  if (status.ok() && options_.reuse_logs && last_log && compactions == 0) {
    assert(logfile_ == nullptr);
    assert(log_ == nullptr);
    assert(mem_ == nullptr);
    uint64_t lfile_size;
    if (env_->GetFileSize(fname, &lfile_size).ok() &&
        env_->NewAppendableFile(fname, &logfile_).ok()) {
      Log(options_.info_log, "Reusing old log %s \n", fname.c_str());
      log_ = new log::Writer(logfile_, lfile_size);
      logfile_number_ = log_number;
      if (mem != nullptr) {
        mem_ = mem;
        mem = nullptr;
      } else {
        // mem can be nullptr if lognum exists but was empty.
        mem_ = new MemTable(internal_comparator_);
        mem_->Ref();
      }
    }
  }

  if (mem != nullptr) {
    // mem did not get reused; compact it.
    if (status.ok()) {
      *save_manifest = true;
      status = WriteLevel0Table(mem, edit, nullptr);
    }
    mem->Unref();
  }

  return status;
}

Status DBImpl::WriteLevel0Table(MemTable* mem, VersionEdit* edit,
                                Version* base) {
  mutex_.AssertHeld();
  const uint64_t start_micros = env_->NowMicros();
  FileMetaData meta;
  meta.number = versions_->NewFileNumber();
  pending_outputs_.insert(meta.number);
  Iterator* iter = mem->NewIterator();
  Log(options_.info_log, "Level-0 table #%llu: started",
      (unsigned long long)meta.number);

  Status s;
  {
    mutex_.Unlock();
    //新生成一个Table_builder负责写文件
    s = BuildTable(dbname_, env_, options_, table_cache_, iter, &meta);
    mutex_.Lock();
  }

  Log(options_.info_log, "Level-0 table #%llu: %lld bytes %s",
      (unsigned long long)meta.number, (unsigned long long)meta.file_size,
      s.ToString().c_str());
  delete iter;
  pending_outputs_.erase(meta.number);

  // Note that if file_size is zero, the file has been deleted and
  // should not be added to the manifest.
  int level = 0;
  if (s.ok() && meta.file_size > 0) {
    const Slice min_user_key = meta.smallest.user_key();
    const Slice max_user_key = meta.largest.user_key();
    if (base != nullptr) {
      level = base->PickLevelForMemTableOutput(min_user_key, max_user_key);
    }
    edit->AddFile(level, meta.number, meta.file_size, meta.smallest,
                  meta.largest);
  }

  CompactionStats stats;
  stats.micros = env_->NowMicros() - start_micros;
  stats.bytes_written = meta.file_size;
  stats_[level].Add(stats);
  return s;
}

void DBImpl::CompactMemTable() {
  mutex_.AssertHeld();
  assert(imm_ != nullptr);

  // Save the contents of the memtable as a new Table
  VersionEdit edit;
  Version* base = versions_->current();
  base->Ref();
  Status s = WriteLevel0Table(imm_, &edit, base);
  base->Unref();

  if (s.ok() && shutting_down_.load(std::memory_order_acquire)) {
    s = Status::IOError("Deleting DB during memtable compaction");
  }

  // Replace immutable memtable with the generated Table
  if (s.ok()) {
    edit.SetPrevLogNumber(0);
    edit.SetLogNumber(logfile_number_);  // Earlier logs no longer needed
    s = versions_->LogAndApply(&edit, &mutex_);
  }

  if (s.ok()) {
    // Commit to the new state
    imm_->Unref();
    imm_ = nullptr;
    has_imm_.store(false, std::memory_order_release);
    DeleteObsoleteFiles();
  } else {
    RecordBackgroundError(s);
  }
}

void DBImpl::CompactRange(const Slice* begin, const Slice* end) {
  int max_level_with_files = 1;
  {
    MutexLock l(&mutex_);
    Version* base = versions_->current();

    // 遍历所有level， 获取重叠的最大层级。
    for (int level = 1; level < config::kNumLevels; level++) {
      if (base->OverlapInLevel(level, begin, end)) {
        max_level_with_files = level;
      }
    }
  }

  // 该操作的目的是什么？为什么不放在OverlapInLevel之前？
  TEST_CompactMemTable();  // TODO(sanjay): Skip if memtable does not overlap

  // 遍历重叠的层级进行major compation
  for (int level = 0; level < max_level_with_files; level++) {
    TEST_CompactRange(level, begin, end);
  }
}

void DBImpl::TEST_CompactRange(int level, const Slice* begin,
                               const Slice* end) {
  assert(level >= 0);
  assert(level + 1 < config::kNumLevels);

  InternalKey begin_storage, end_storage;

  ManualCompaction manual;
  manual.level = level;
  manual.done = false;
  if (begin == nullptr) {
    manual.begin = nullptr;
  } else {
    begin_storage = InternalKey(*begin, kMaxSequenceNumber, kValueTypeForSeek);
    manual.begin = &begin_storage;
  }
  if (end == nullptr) {
    manual.end = nullptr;
  } else {
    end_storage = InternalKey(*end, 0, static_cast<ValueType>(0));
    manual.end = &end_storage;
  }

  MutexLock l(&mutex_);
  while (!manual.done && !shutting_down_.load(std::memory_order_acquire) &&
         bg_error_.ok()) {
    if (manual_compaction_ == nullptr) {  // Idle
      manual_compaction_ = &manual;
      MaybeScheduleCompaction();
    } else {  // Running either my compaction or another compaction.
      background_work_finished_signal_.Wait();
    }
  }
  if (manual_compaction_ == &manual) {
    // Cancel my manual compaction since we aborted early for some reason.
    manual_compaction_ = nullptr;
  }
}

Status DBImpl::TEST_CompactMemTable() {
  // nullptr batch means just wait for earlier writes to be done
  Status s = Write(WriteOptions(), nullptr);
  if (s.ok()) {
    // Wait until the compaction completes
    MutexLock l(&mutex_);
    while (imm_ != nullptr && bg_error_.ok()) {
      background_work_finished_signal_.Wait();
    }
    if (imm_ != nullptr) {
      s = bg_error_;
    }
  }
  return s;
}

void DBImpl::RecordBackgroundError(const Status& s) {
  mutex_.AssertHeld();
  if (bg_error_.ok()) {
    bg_error_ = s;
    background_work_finished_signal_.SignalAll();
  }
}

void DBImpl::MaybeScheduleCompaction() {
  mutex_.AssertHeld();
  if (background_compaction_scheduled_) {
    // Already scheduled
  } else if (shutting_down_.load(std::memory_order_acquire)) {
    // DB is being deleted; no more background compactions
  } else if (!bg_error_.ok()) {
    // Already got an error; no more changes
  } else if (imm_ == nullptr && manual_compaction_ == nullptr &&
             !versions_->NeedsCompaction()) {
    // No work to be done
  } else {
    background_compaction_scheduled_ = true;
    env_->Schedule(&DBImpl::BGWork, this);
  }
}

void DBImpl::BGWork(void* db) {
  reinterpret_cast<DBImpl*>(db)->BackgroundCall();
}

void DBImpl::BackgroundCall() {
  MutexLock l(&mutex_);
  assert(background_compaction_scheduled_);
  if (shutting_down_.load(std::memory_order_acquire)) {
    // No more background work when shutting down.
  } else if (!bg_error_.ok()) {
    // No more background work after a background error.
  } else {
    BackgroundCompaction();
  }

  background_compaction_scheduled_ = false;

  // Previous compaction may have produced too many files in a level,
  // so reschedule another compaction if needed.
  MaybeScheduleCompaction();
  background_work_finished_signal_.SignalAll();
}

// 统计key的访问频率结构体
typedef struct{
	std::string key;
  std::string value;
	int cnt;      //key出现的频率
}Keys;
const int key_num = 1024000; //key数量上限
Keys keys_all[key_num]; // 所有的key

void DBImpl::BackgroundCompaction() {
  mutex_.AssertHeld();
  //如果immutable不为空，需要将immutable dump到level 0
  if (imm_ != nullptr) {
    // 提取热数据
    int key_index = 0;// 实际所有key的数量
    int max_freq = 0;// 最大访问频率
    int i, j, k;
    int neww=1;//neww标志位，判断是否为新key
    Iterator* iter = imm_->NewIterator(); // 遍历冷数据表
    iter->SeekToFirst();
    Slice pre_key_mem = Slice("~!@#$%^&*");
    for (; iter->Valid(); iter->Next()) {
      neww=1;
      if ((pre_key_mem != Slice("~!@#$%^&*")) &&
          (ExtractUserKey(iter->key()) == pre_key_mem))
      {
        keys_all[key_index].cnt += 1;
        neww = 0;
        if (max_freq < keys_all[key_index].cnt)
        {
          max_freq = keys_all[key_index].cnt;
        }
      } else if (pre_key_mem != Slice("~!@#$%^&*"))
      {
        key_index++;
      }
      if (neww) {
        // 复制key，第一次拷贝需要复制value（获取最新值）
        keys_all[key_index].key = ExtractUserKey(iter->key()).ToString();
        keys_all[key_index].value = iter->value().ToString();
        // 新key数量+1
        keys_all[key_index].cnt = 1;
      }
      pre_key_mem = ExtractUserKey(iter->key());
    }
    // 调试代码
    //std::string tmp = "======";
    //os1 << tmp << std::endl;

    for(i = 0; i <= key_index; i++)
    {
      // 调试代码
      //os2<<"put:mem_hot_ = new Skiplist_(), node_count: " + std::to_string(mem_hot_->node_count) << std::endl;
      // 将访问频率大于1，并且访问频率在前20%的数据插入热数据表中，同时插入的数据量小于1000个
      // if (keys_all[i].cnt > 1 && keys_all[i].cnt > (int)(max_freq * 0.8) && hot_num < 1000)
      if (keys_all[i].cnt > 1)
      {
        mem_hot_->set(keys_all[i].key, keys_all[i].value);
        // 调试代码
        // os1<<"put:mem_hot_->set, key: " + keys_all[i].key + ", value: " + keys_all[i].value + ", node_count: " + std::to_string(mem_hot_->node_count) << std::endl;
        
        if (mem_hot_->node_count > options_.write_buffer_count_hot)
        {
          if (mem_level2_ != nullptr)
          {
            imm_level2_ = mem_level2_;
          }
          if (mem_level1_ != nullptr)
          {
            mem_level2_ = mem_level1_;
          }
          if (mem_level0_ != nullptr)
          {
            mem_level1_ = mem_level0_;
          }
          mem_level0_ = mem_hot_;
          mem_hot_ = new Skiplist_();
        }
      }
    }
    // immutable dump到磁盘
    CompactMemTable();

    if (imm_level2_ != nullptr && imm_ == nullptr)
    {
      // 调试代码
      // std::string tmp = "======";
      // os3 << tmp << std::endl;

      imm_ = new MemTable(internal_comparator_);
      imm_->Ref();
      WriteBatch updates_imm;
      for (int i = imm_level2_->current_level-1; i >=0; --i)
      {
        Skiplist_::node_ptr p = imm_level2_->header;
        Skiplist_::node_ptr next = NULL;
        while ((next = p->get_level(i)) != NULL)
        {
          updates_imm.Put(Slice(next->key), Slice(next->value));
          WriteBatchInternal::InsertInto(&updates_imm, imm_);
          // 调试代码
          // os3<<"put:imm_level2_ != nullptr, WriteBatchInternal::InsertInto(&updates_imm, imm_)" << std::endl;
          
          p = next;
        }
      }
      imm_level2_->release();
      imm_level2_ = nullptr;
      // level2的数据dump到磁盘
      CompactMemTable();
    }
    return;
  }

  Compaction* c;
  bool is_manual = (manual_compaction_ != nullptr);
  InternalKey manual_end;
  if (is_manual) {  //Manual Compaction
    ManualCompaction* m = manual_compaction_;
    c = versions_->CompactRange(m->level, m->begin, m->end);
    m->done = (c == nullptr);
    if (c != nullptr) {
      manual_end = c->input(0, c->num_input_files(0) - 1)->largest;
    }
    Log(options_.info_log,
        "Manual compaction at level-%d from %s .. %s; will stop at %s\n",
        m->level, (m->begin ? m->begin->DebugString().c_str() : "(begin)"),
        (m->end ? m->end->DebugString().c_str() : "(end)"),
        (m->done ? "(end)" : manual_end.DebugString().c_str()));
  } else {
    c = versions_->PickCompaction();  //选取压缩方式，Size/Seek Compaction
  }

  Status status;
  if (c == nullptr) {
    // Nothing to do
  } else if (!is_manual && c->IsTrivialMove()) {
    // // trivial compaction，直接将当前level上需要compaction的sst移动到下一层，不需要进行合并操作.
    // Move file to next level
    assert(c->num_input_files(0) == 1);
    FileMetaData* f = c->input(0, 0);
    c->edit()->DeleteFile(c->level(), f->number);
    c->edit()->AddFile(c->level() + 1, f->number, f->file_size, f->smallest,
                       f->largest);
    status = versions_->LogAndApply(c->edit(), &mutex_); //写入version
    if (!status.ok()) {
      RecordBackgroundError(status);
    }
    VersionSet::LevelSummaryStorage tmp;
    Log(options_.info_log, "Moved #%lld to level-%d %lld bytes %s: %s\n",
        static_cast<unsigned long long>(f->number), c->level() + 1,
        static_cast<unsigned long long>(f->file_size),
        status.ToString().c_str(), versions_->LevelSummary(&tmp));
  } else {
    CompactionState* compact = new CompactionState(c);
    status = DoCompactionWork(compact);  // 真正进行compaction的地方
    if (!status.ok()) {
      RecordBackgroundError(status);
    }
    CleanupCompaction(compact);  //清理compact过程中的临时变量
    c->ReleaseInputs();          //清除输入文件描述符
    DeleteObsoleteFiles();       //删除无引用的文件
  }
  delete c;

  if (status.ok()) {
    // Done
  } else if (shutting_down_.load(std::memory_order_acquire)) {
    // Ignore compaction errors found during shutting down
  } else {
    Log(options_.info_log, "Compaction error: %s", status.ToString().c_str());
  }

  if (is_manual) {
    ManualCompaction* m = manual_compaction_;
    if (!status.ok()) {
      m->done = true;
    }
    if (!m->done) {
      // We only compacted part of the requested range.  Update *m
      // to the range that is left to be compacted.
      m->tmp_storage = manual_end;
      m->begin = &m->tmp_storage;
    }
    manual_compaction_ = nullptr;
  }
}

void DBImpl::CleanupCompaction(CompactionState* compact) {
  mutex_.AssertHeld();
  if (compact->builder != nullptr) {
    // May happen if we get a shutdown call in the middle of compaction
    compact->builder->Abandon();
    delete compact->builder;
  } else {
    assert(compact->outfile == nullptr);
  }
  delete compact->outfile;
  for (size_t i = 0; i < compact->outputs.size(); i++) {
    const CompactionState::Output& out = compact->outputs[i];
    pending_outputs_.erase(out.number);
  }
  delete compact;
}

Status DBImpl::OpenCompactionOutputFile(CompactionState* compact) {
  assert(compact != nullptr);
  assert(compact->builder == nullptr);
  uint64_t file_number;
  {
    mutex_.Lock();
    file_number = versions_->NewFileNumber();
    pending_outputs_.insert(file_number);
    CompactionState::Output out;
    out.number = file_number;
    out.smallest.Clear();
    out.largest.Clear();
    compact->outputs.push_back(out);
    mutex_.Unlock();
  }

  // Make the output file
  std::string fname = TableFileName(dbname_, file_number);
  Status s = env_->NewWritableFile(fname, &compact->outfile);
  if (s.ok()) {
    compact->builder = new TableBuilder(options_, compact->outfile);
  }
  return s;
}

Status DBImpl::FinishCompactionOutputFile(CompactionState* compact,
                                          Iterator* input) {
  assert(compact != nullptr);
  assert(compact->outfile != nullptr);
  assert(compact->builder != nullptr);

  const uint64_t output_number = compact->current_output()->number;
  assert(output_number != 0);

  // Check for iterator errors
  Status s = input->status();
  const uint64_t current_entries = compact->builder->NumEntries();
  if (s.ok()) {
    s = compact->builder->Finish();
  } else {
    compact->builder->Abandon();
  }
  const uint64_t current_bytes = compact->builder->FileSize();
  compact->current_output()->file_size = current_bytes;
  compact->total_bytes += current_bytes;
  delete compact->builder;
  compact->builder = nullptr;

  // Finish and check for file errors
  if (s.ok()) {
    s = compact->outfile->Sync();
  }
  if (s.ok()) {
    s = compact->outfile->Close();
  }
  delete compact->outfile;
  compact->outfile = nullptr;

  if (s.ok() && current_entries > 0) {
    // Verify that the table is usable
    Iterator* iter =
        table_cache_->NewIterator(ReadOptions(), output_number, current_bytes);
    s = iter->status();
    delete iter;
    if (s.ok()) {
      Log(options_.info_log, "Generated table #%llu@%d: %lld keys, %lld bytes",
          (unsigned long long)output_number, compact->compaction->level(),
          (unsigned long long)current_entries,
          (unsigned long long)current_bytes);
    }
  }
  return s;
}

Status DBImpl::InstallCompactionResults(CompactionState* compact) {
  mutex_.AssertHeld();
  Log(options_.info_log, "Compacted %d@%d + %d@%d files => %lld bytes",
      compact->compaction->num_input_files(0), compact->compaction->level(),
      compact->compaction->num_input_files(1), compact->compaction->level() + 1,
      static_cast<long long>(compact->total_bytes));

  // compaction完成后被合并的sst在新版本中就没有用了，将这些文件加入到VersionEdit的deleted_files_中
  // Add compaction outputs
  compact->compaction->AddInputDeletions(compact->compaction->edit());

  // 将新生成的sst文件加入到VersionEdit的new_files_中
  const int level = compact->compaction->level();
  for (size_t i = 0; i < compact->outputs.size(); i++) {
    const CompactionState::Output& out = compact->outputs[i];
    compact->compaction->edit()->AddFile(level + 1, out.number, out.file_size,
                                         out.smallest, out.largest);
  }

  // LogAndApply会根据VerionEdit中deleted_files_和new_files_生成一个新的Version
  return versions_->LogAndApply(compact->compaction->edit(), &mutex_);
}

Status DBImpl::DoCompactionWork(CompactionState* compact) {
  const uint64_t start_micros = env_->NowMicros();
  int64_t imm_micros = 0;  // Micros spent doing imm_ compactions

  Log(options_.info_log, "Compacting %d@%d + %d@%d files",
      compact->compaction->num_input_files(0), compact->compaction->level(),
      compact->compaction->num_input_files(1),
      compact->compaction->level() + 1);

  assert(versions_->NumLevelFiles(compact->compaction->level()) > 0);
  assert(compact->builder == nullptr);
  assert(compact->outfile == nullptr);

  // 将snapshot相关的内容记录到compact信息中
  if (snapshots_.empty()) {
    compact->smallest_snapshot = versions_->LastSequence();
  } else {
    compact->smallest_snapshot = snapshots_.oldest()->sequence_number();
  }

  // 这里生成一个MergingIterator，相当于在遍历要合并的sst文件时，同时进行多路归并排序
  // MergingIterator内部维护了n个Iterator，每个Iterator指向一个sst，进行迭代时，MergingIterator
  // 会找所有Iterators所指key中的最小那个，这样就完成了多路归并排序
  Iterator* input = versions_->MakeInputIterator(compact->compaction);

  // Release mutex while we're actually doing the compaction work
  mutex_.Unlock();

  input->SeekToFirst();
  Status status;
  ParsedInternalKey ikey;
  std::string current_user_key;
  bool has_current_user_key = false;
  SequenceNumber last_sequence_for_key = kMaxSequenceNumber;
  while (input->Valid() && !shutting_down_.load(std::memory_order_acquire)) {
    // Prioritize immutable compaction work
    if (has_imm_.load(std::memory_order_relaxed)) {
      const uint64_t imm_start = env_->NowMicros();
      mutex_.Lock();
      if (imm_ != nullptr) {
        CompactMemTable();
        // Wake up MakeRoomForWrite() if necessary.
        background_work_finished_signal_.SignalAll();
      }
      mutex_.Unlock();
      imm_micros += (env_->NowMicros() - imm_start);
    }

    Slice key = input->key();
    if (compact->compaction->ShouldStopBefore(key) &&
        compact->builder != nullptr) {
      status = FinishCompactionOutputFile(compact, input);
      if (!status.ok()) {
        break;
      }
    }

    // Handle key/value, add to state, etc.
    bool drop = false;
    if (!ParseInternalKey(key, &ikey)) {
      // Do not hide error keys
      current_user_key.clear();
      has_current_user_key = false;
      last_sequence_for_key = kMaxSequenceNumber;
    } else {
      if (!has_current_user_key ||
          user_comparator()->Compare(ikey.user_key, Slice(current_user_key)) !=
              0) {
        // First occurrence of this user key
        current_user_key.assign(ikey.user_key.data(), ikey.user_key.size());
        has_current_user_key = true;
        last_sequence_for_key = kMaxSequenceNumber;
      }

      if (last_sequence_for_key <= compact->smallest_snapshot) {
        // Hidden by an newer entry for same user key
        drop = true;  // (A)
      } else if (ikey.type == kTypeDeletion &&
                 ikey.sequence <= compact->smallest_snapshot &&
                 compact->compaction->IsBaseLevelForKey(ikey.user_key)) {
        // For this user key:
        // (1) there is no data in higher levels
        // (2) data in lower levels will have larger sequence numbers
        // (3) data in layers that are being compacted here and have
        //     smaller sequence numbers will be dropped in the next
        //     few iterations of this loop (by rule (A) above).
        // Therefore this deletion marker is obsolete and can be dropped.
        drop = true;
      }

      last_sequence_for_key = ikey.sequence;
    }
#if 0
    Log(options_.info_log,
        "  Compact: %s, seq %d, type: %d %d, drop: %d, is_base: %d, "
        "%d smallest_snapshot: %d",
        ikey.user_key.ToString().c_str(),
        (int)ikey.sequence, ikey.type, kTypeValue, drop,
        compact->compaction->IsBaseLevelForKey(ikey.user_key),
        (int)last_sequence_for_key, (int)compact->smallest_snapshot);
#endif

    if (!drop) {
      // Open output file if necessary
      if (compact->builder == nullptr) {
        status = OpenCompactionOutputFile(compact);
        if (!status.ok()) {
          break;
        }
      }
      if (compact->builder->NumEntries() == 0) {
        compact->current_output()->smallest.DecodeFrom(key);
      }
      compact->current_output()->largest.DecodeFrom(key);
      compact->builder->Add(key, input->value());

      // Close output file if it is big enough
      if (compact->builder->FileSize() >=
          compact->compaction->MaxOutputFileSize()) {
        status = FinishCompactionOutputFile(compact, input);
        if (!status.ok()) {
          break;
        }
      }
    }

    input->Next();
  }

  if (status.ok() && shutting_down_.load(std::memory_order_acquire)) {
    status = Status::IOError("Deleting DB during compaction");
  }
  if (status.ok() && compact->builder != nullptr) {
    status = FinishCompactionOutputFile(compact, input);
  }
  if (status.ok()) {
    status = input->status();
  }
  delete input;
  input = nullptr;

  CompactionStats stats;
  stats.micros = env_->NowMicros() - start_micros - imm_micros;
  for (int which = 0; which < 2; which++) {
    for (int i = 0; i < compact->compaction->num_input_files(which); i++) {
      stats.bytes_read += compact->compaction->input(which, i)->file_size;
    }
  }
  for (size_t i = 0; i < compact->outputs.size(); i++) {
    stats.bytes_written += compact->outputs[i].file_size;
  }

  mutex_.Lock();
  stats_[compact->compaction->level() + 1].Add(stats);

  if (status.ok()) {
    status = InstallCompactionResults(compact);   // 将新生成的sst 加入到Version中
  }
  if (!status.ok()) {
    RecordBackgroundError(status);
  }
  VersionSet::LevelSummaryStorage tmp;
  Log(options_.info_log, "compacted to: %s", versions_->LevelSummary(&tmp));
  return status;
}

namespace {

struct IterState {
  port::Mutex* const mu;
  Version* const version GUARDED_BY(mu);
  MemTable* const mem GUARDED_BY(mu);
  MemTable* const imm GUARDED_BY(mu);

  IterState(port::Mutex* mutex, MemTable* mem, MemTable* imm, Version* version)
      : mu(mutex), version(version), mem(mem), imm(imm) {}
};

static void CleanupIteratorState(void* arg1, void* arg2) {
  IterState* state = reinterpret_cast<IterState*>(arg1);
  state->mu->Lock();
  state->mem->Unref();
  if (state->imm != nullptr) state->imm->Unref();
  state->version->Unref();
  state->mu->Unlock();
  delete state;
}

}  // anonymous namespace

Iterator* DBImpl::NewInternalIterator(const ReadOptions& options,
                                      SequenceNumber* latest_snapshot,
                                      uint32_t* seed) {
  mutex_.Lock();
  *latest_snapshot = versions_->LastSequence();

  // Collect together all needed child iterators
  std::vector<Iterator*> list;
  list.push_back(mem_->NewIterator());
  mem_->Ref();
  if (imm_ != nullptr) {
    list.push_back(imm_->NewIterator());
    imm_->Ref();
  }
  versions_->current()->AddIterators(options, &list);
  Iterator* internal_iter =
      NewMergingIterator(&internal_comparator_, &list[0], list.size());
  versions_->current()->Ref();

  IterState* cleanup = new IterState(&mutex_, mem_, imm_, versions_->current());
  internal_iter->RegisterCleanup(CleanupIteratorState, cleanup, nullptr);

  *seed = ++seed_;
  mutex_.Unlock();
  return internal_iter;
}

Iterator* DBImpl::TEST_NewInternalIterator() {
  SequenceNumber ignored;
  uint32_t ignored_seed;
  return NewInternalIterator(ReadOptions(), &ignored, &ignored_seed);
}

int64_t DBImpl::TEST_MaxNextLevelOverlappingBytes() {
  MutexLock l(&mutex_);
  return versions_->MaxNextLevelOverlappingBytes();
}

Status DBImpl::Get(const ReadOptions& options, const Slice& key,
                   std::string* value) {
  Status s;
  MutexLock l(&mutex_); //获取互斥锁。

  // 获取本次读操作的 Sequence Number：
  // 如果 ReadOptions 参数的 snaphot 不为空，则使用这个 snapshot 的 Sequence Number；
  // 否则，默认使用 LastSequence（LastSequence 会在每次写操作后更新）。
  SequenceNumber snapshot;
  if (options.snapshot != nullptr) {
    snapshot =
        static_cast<const SnapshotImpl*>(options.snapshot)->sequence_number();
  } else {
    snapshot = versions_->LastSequence();
  }

  // MemTable， Immutable Memtable 和 Current Version 增加引用计数，避免在读取过程中被后台线程进行 Compaction 时“垃圾回收”了。
  // Version 主要用来维护 SST 文件的版本信息。
  Skiplist_* mem_hot = mem_hot_;
  Skiplist_* mem_level0 = mem_level0_;
  Skiplist_* mem_level1 = mem_level1_;
  Skiplist_* mem_level2 = mem_level2_;
  MemTable* mem = mem_;
  MemTable* imm = imm_;
  Version* current = versions_->current();
  mem->Ref();
  if (imm != nullptr) imm->Ref();
  current->Ref();

  bool have_stat_update = false;
  Version::GetStats stats;

  // Unlock while reading from files and memtables
  {
    // 释放互斥锁
    mutex_.Unlock();
    // First look in the memtable, then in the immutable memtable (if any).
    // 查找顺序：
    // 1、从 MemTable 查找 。
    // 2、从 Immutable Memtable 查找。
    // 3、从 SSTable 文件查找。
    LookupKey lkey(key, snapshot);
    if (mem_hot != nullptr && mem_hot->get(key.ToString(), *value)) {
      // 调试代码
      //os4<<"get:mem_hot->get(key.ToString(), value: " + *value << std::endl;
      // Done
    } else if (mem_level0 != nullptr && mem_level0->get(key.ToString(), *value)) {
      //os4<<"get:mem_level0->get(key.ToString(), value: " + *value << std::endl;
      // Done
    } else if (mem_level1 != nullptr && mem_level1->get(key.ToString(), *value)) {
      //os4<<"get:mem_level1->get(key.ToString(), value: " + *value << std::endl;
      // Done
    } else if (mem_level2 != nullptr && mem_level2->get(key.ToString(), *value)) {
      //os4<<"get:mem_level2->get(key.ToString(), value: " + *value << std::endl;
      // Done
    } else if (mem->Get(lkey, value, &s)) {
      //os4<<"get:mem->Get(lkey, value, &s), value: " + *value << std::endl;
      // Done
    } else if (imm != nullptr && imm->Get(lkey, value, &s)) {
      //os4<<"get:imm->Get(lkey, value, &s), value: " + *value << std::endl;
      // Done
    } else {
      s = current->Get(options, lkey, value, &stats);
      have_stat_update = true;
      // mem_hot_->set(key.ToString(), *value);
      // // 调试代码
      // os7<<"get:disk, key: " + key.ToString() << ", value" << *value << std::endl;
      // if (mem_hot_->node_count > options_.write_buffer_count_hot)
      // {
      //   if (mem_level2_ != nullptr)
      //   {
      //     imm_level2_ = mem_level2_;
      //   }
      //   if (mem_level1_ != nullptr)
      //   {
      //     mem_level2_ = mem_level1_;
      //   }
      //   if (mem_level0_ != nullptr)
      //   {
      //     mem_level1_ = mem_level0_;
      //   }
      //   mem_level0_ = mem_hot_;
      //   mem_hot_ = new Skiplist_();
      //   // 调试代码
      //   // os4<<"get:mem_hot_ = new Skiplist_(), node_count: " + std::to_string(mem_hot_->node_count) << std::endl;
      // }
      // if (imm_level2_ != nullptr)
      // {
      //   imm_level2_->release();
      //   imm_level2_ = nullptr;
      // }
    }
    
    //获取互斥锁
    mutex_.Lock(); 
  }

  // 更新 SST 文件的统计信息，根据统计结果决定是否调度后台 Compaction。
  if (have_stat_update && current->UpdateStats(stats)) {
    MaybeScheduleCompaction();
  }
  // MemTable, Immutable Memtable 和 Current Version 减少引用计数。
  mem->Unref();
  if (imm != nullptr) imm->Unref();
  current->Unref();

  // 释放锁（由析构函数完成），返回结果。
  return s;
}

Iterator* DBImpl::NewIterator(const ReadOptions& options) {
  SequenceNumber latest_snapshot;
  uint32_t seed;
  Iterator* iter = NewInternalIterator(options, &latest_snapshot, &seed);
  return NewDBIterator(this, user_comparator(), iter,
                       (options.snapshot != nullptr
                            ? static_cast<const SnapshotImpl*>(options.snapshot)
                                  ->sequence_number()
                            : latest_snapshot),
                       seed);
}

void DBImpl::RecordReadSample(Slice key) {
  MutexLock l(&mutex_);
  if (versions_->current()->RecordReadSample(key)) {
    MaybeScheduleCompaction();
  }
}

const Snapshot* DBImpl::GetSnapshot() {
  MutexLock l(&mutex_);
  return snapshots_.New(versions_->LastSequence());
}

void DBImpl::ReleaseSnapshot(const Snapshot* snapshot) {
  MutexLock l(&mutex_);
  snapshots_.Delete(static_cast<const SnapshotImpl*>(snapshot));
}

// Convenience methods
Status DBImpl::Put(const WriteOptions& options, const Slice& key, const Slice& val) {
  // 判断是否为热数据：使用Get检查数据是否在热数据表中，如果在就直接写入，如果不在就写入冷数据表
  Status s;
  std::string value;
  if (mem_hot_ != nullptr && mem_hot_->get(key.ToString(), value, val.ToString(), log_hot_)) {
    // 调试代码
    //os5<<"DBImpl::Put, mem_hot_->get(key.ToString(), value, val.ToString()), key: " + key.ToString() + ", value: " + value << std::endl;
    // 写入热数据表
    return s.OK();
  }
  else if (mem_level0_ != nullptr && mem_level0_->get(key.ToString(), value, val.ToString())) {
    // 写入Level0
    //os5<<"DBImpl::Put, mem_level0_->get(key.ToString(), value, val.ToString()), key: " + key.ToString() + ", value: " + value << std::endl;
    return s.OK();
  }
  else if (mem_level1_ != nullptr && mem_level1_->get(key.ToString(), value, val.ToString())) {
    // 写入Level1
    //os5<<"DBImpl::Put, mem_level1_->get(key.ToString(), value, val.ToString()), key: " + key.ToString() + ", value: " + value << std::endl;
    return s.OK();
  }
  else if (mem_level2_ != nullptr && mem_level2_->get(key.ToString(), value, val.ToString())) {
    // 写入Level2
    //os5<<"DBImpl::Put, mem_level2_->get(key.ToString(), value, val.ToString()), key: " + key.ToString() + ", value: " + value << std::endl;
    return s.OK();
  }
  else
  {
    //os6<<"DB::Put(options, key, val), key: " + key.ToString() << ", val: " + val.ToString() << std::endl;
    // 写入冷数据表
    return DB::Put(options, key, val);
  }
}

Status DBImpl::Delete(const WriteOptions& options, const Slice& key) {
  Status s;
  // 判断是否为热数据：使用Get检查数据是否在热数据表中，如果在就直接写入，如果不在就写入冷数据表
  std::string value;
  if (mem_hot_ != nullptr && mem_hot_->get(key.ToString(), value, "", log_hot_)) {
    // 写入热数据表
    //os5<<"DBImpl::Delete, mem_hot_->get(key.ToString(), value, val.ToString()), key: " + key.ToString() + ", value: " + value << std::endl;
    return s.OK();
  }
  else if (mem_level0_ != nullptr && mem_level0_->get(key.ToString(), value, "")) {
    // 写入Level0
    //os5<<"DBImpl::Delete, mem_level0_->get(key.ToString(), value, val.ToString()), key: " + key.ToString() + ", value: " + value << std::endl;
    return s.OK();
  }
  else if (mem_level1_ != nullptr && mem_level1_->get(key.ToString(), value, "")) {
    // 写入Level1
    //os5<<"DBImpl::Delete, mem_level1_->get(key.ToString(), value, val.ToString()), key: " + key.ToString() + ", value: " + value << std::endl;
    return s.OK();
  }
  else if (mem_level2_ != nullptr && mem_level2_->get(key.ToString(), value, "")) {
    // 写入Level2
    //os5<<"DBImpl::Delete, mem_level2_->get(key.ToString(), value, val.ToString()), key: " + key.ToString() + ", value: " + value << std::endl;
    return s.OK();
  }
  else
  {
   // os6<<"DB::Delete(options, key, val), key: " + key.ToString() << std::endl;
    // 写入冷数据表
    return DB::Delete(options, key);
  }
}

// 处理过程
// 1. 队列化请求
//     mutex l上锁之后, 到了"w.cv.Wait()"的时候, 会先释放锁等待, 然后收到signal时再次上锁. 
//     这段代码的作用就是多线程在提交任务的时候,一个接一个push_back进队列. 
//     但只有位于队首的线程有资格继续运行下去. 目的是把多个写请求合并成一个大batch提升效率.
// 2. 写入的前期检查和保证
// 3. 按格式组装数据为二进制
// 4. 写入log文件和memtable
// 5. 唤醒队列的其他人去干活，自己返回
Status DBImpl::Write(const WriteOptions& options, WriteBatch* updates) {
  Writer w(&mutex_);
  w.batch = updates;
  w.sync = options.sync;
  w.done = false;

  //串行化writer。如果有其他writer在执行则进入队列等待被唤醒执行
  MutexLock l(&mutex_);
  writers_.push_back(&w);
  while (!w.done && &w != writers_.front()) {
    w.cv.Wait();
  }

  //writer的任务被其他writer帮忙执行了，则返回。BuildBatchGroup会有合并写的操作。
  if (w.done) {
    return w.status;
  }

  // May temporarily unlock and wait.
  // 写入前的各种检查。是否该停写,是否该切memtable,是否该compact
  Status status = MakeRoomForWrite(updates == nullptr);
  
  // 获取本次写入的版本号,其实就是个uint64
  uint64_t last_sequence = versions_->LastSequence();
  Writer* last_writer = &w;
  //这里writer还是队列中第一个,由于下面会队列前面的writers也可能合并起来,所以last_writer指针会指向被合并的最后一个writer
  if (status.ok() && updates != nullptr) {  // nullptr batch is for compactions
    WriteBatch* updates = BuildBatchGroup(&last_writer); //这里会把writers队列中的其他适合的写操作一起执行
    WriteBatchInternal::SetSequence(updates, last_sequence + 1); //把版本号写入batch中
    last_sequence += WriteBatchInternal::Count(updates); //updates如果合并了n条操作,版本号也会跳跃n

    // Add to log and apply to memtable.  We can release the lock
    // during this phase since &w is currently responsible for logging
    // and protects against concurrent loggers and concurrent writes
    // into mem_.
    {
      mutex_.Unlock();
      status = log_->AddRecord(WriteBatchInternal::Contents(updates));  //第一步写入log，用于故障恢复，防止数据丢失。
      bool sync_error = false;
      if (status.ok() && options.sync) {
        status = logfile_->Sync();
        if (!status.ok()) {
          sync_error = true;
        }
      }
      if (status.ok()) {
        status = WriteBatchInternal::InsertInto(updates, mem_); //插入memtable了
      }
      mutex_.Lock();
      if (sync_error) {
        // The state of the log file is indeterminate: the log record we
        // just added may or may not show up when the DB is re-opened.
        // So we force the DB into a mode where all future writes fail.
        RecordBackgroundError(status);
      }
    }
    if (updates == tmp_batch_) tmp_batch_->Clear();

    versions_->SetLastSequence(last_sequence);
  }

  // 将处理完的任务从队列里取出，并置状态为done，然后通知对应的CondVar启动。
  while (true) {
    Writer* ready = writers_.front();
    writers_.pop_front();
    if (ready != &w) {
      ready->status = status;
      ready->done = true;
      ready->cv.Signal();
    }
    if (ready == last_writer) break; //直到last_writer通知为止。
  }

  // Notify new head of write queue。通知队列中第一个writer干活。
  if (!writers_.empty()) {
    writers_.front()->cv.Signal();
  }

  return status;
}

// REQUIRES: Writer list must be non-empty
// REQUIRES: First writer must have a non-null batch

// 作用：writer会被队列化，而只有排在第一位的writer才会往下执行。而writer有可能会把排在它后面的writer的活也拿过来一起干了。
// 机制：遍历队列后面的writer们，一直到遇到帮不了忙的writer就返回了。那么帮不帮忙的标准是啥？两个标准：
//  1.sync类型是否一样（我不需要马上flush到磁盘而你要，你的活还是自己干吧）
//  2.写入的数据量是不是过大了？（避免单次写入数据量太大）
// 只要没有符合这两个限制条件，就可以帮忙，合并多条write操作为一条操作
WriteBatch* DBImpl::BuildBatchGroup(Writer** last_writer) {
  mutex_.AssertHeld();
  assert(!writers_.empty());
  Writer* first = writers_.front();
  WriteBatch* result = first->batch;
  assert(result != nullptr);

  // 计算要自己要写入的byte大小
  size_t size = WriteBatchInternal::ByteSize(first->batch);

  // Allow the group to grow up to a maximum size, but if the
  // original write is small, limit the growth so we do not slow
  // down the small write too much.
  size_t max_size = 1 << 20;
  if (size <= (128 << 10)) {
    max_size = size + (128 << 10);
  }

  *last_writer = first;
  std::deque<Writer*>::iterator iter = writers_.begin();
  ++iter;  // Advance past "first"
  for (; iter != writers_.end(); ++iter) {
    Writer* w = *iter;
    if (w->sync && !first->sync) {  //sync类型不同,你的活我不帮你了
      // Do not include a sync write into a batch handled by a non-sync write.
      break;
    }

    if (w->batch != nullptr) {
      size += WriteBatchInternal::ByteSize(w->batch);
      if (size > max_size) {  //这个帮忙数据量过大了,我也不帮忙了
        // Do not make batch too big
        break;
      }

      // Append to *result
      if (result == first->batch) {
        // Switch to temporary batch instead of disturbing caller's batch
        result = tmp_batch_;
        assert(WriteBatchInternal::Count(result) == 0);
        WriteBatchInternal::Append(result, first->batch); 
      }
      WriteBatchInternal::Append(result, w->batch); //来吧,你的活我可以帮你干了
    }
    *last_writer = w;
  }
  return result;
}

// REQUIRES: mutex_ is held
// REQUIRES: this thread is currently at the front of the writer queue
// 1. 后台任务有无出现错误？出现错误直接返回错误。（compaction是后台线程执行的）
// 2. level 0 的文件数超过8个，则等待1s再继续执行，因为level 0的文件数目需要严格控制
// 3. 如果memtable的大小小于4MB（默认值，可以修改），直接返回可以插入；
// 4. 到达4说明memtable已经满了，这时候需要切换为Imuable memtable。所以这时候需要等待旧的Imuable memtable compact到level 0，进入等待
// 5. 到达5说明旧的Imuable memtable已经compact到level 0了，这时候假如level 0的文件数目到达了12个，也需要等待
// 6. 到达6说明旧的Imuable memtable已经compact到磁盘了，level 0的文件数目也符合要求，这时候当前的memtable可以转换成Imuable memtable，并启动后台compact。同时生成新的memtable、log用于数据的写入。
Status DBImpl::MakeRoomForWrite(bool force) {
  mutex_.AssertHeld();
  assert(!writers_.empty());
  bool allow_delay = !force;  //allow_delay代表compaction可以延后
  Status s;
  while (true) {
    if (!bg_error_.ok()) {
      // Yield previous error 如果后台任务已经出错,直接返回错误
      s = bg_error_;
      break;
    } else if (allow_delay && versions_->NumLevelFiles(0) >=
                                  config::kL0_SlowdownWritesTrigger) {
      // level0的文件数限制超过8,睡眠1ms,简单等待后台任务执行。写入writer线程向压缩线程转让cpu。
      // We are getting close to hitting a hard limit on the number of
      // L0 files.  Rather than delaying a single write by several
      // seconds when we hit the hard limit, start delaying each
      // individual write by 1ms to reduce latency variance.  Also,
      // this delay hands over some CPU to the compaction thread in
      // case it is sharing the same core as the writer.
      mutex_.Unlock();
      env_->SleepForMicroseconds(1000);
      allow_delay = false;  // Do not delay a single write more than once
      mutex_.Lock();
    } else if (!force &&
               (mem_->ApproximateMemoryUsage() <= options_.write_buffer_size)) {
      // There is room in current memtable
      // 当前memtable，还有空间继续写入
      break;
    } else if (imm_ != nullptr) {
      // We have filled up the current memtable, but the previous
      // one is still being compacted, so we wait.
      // 等待之前的imuable memtable完成compact到level0
      Log(options_.info_log, "Current memtable full; waiting...\n");
      background_work_finished_signal_.Wait();
    } else if (versions_->NumLevelFiles(0) >= config::kL0_StopWritesTrigger) {
      // There are too many level-0 files.
      Log(options_.info_log, "Too many L0 files; waiting...\n");
      background_work_finished_signal_.Wait();
    } else {
      // Attempt to switch to a new memtable and trigger compaction of old
      // 当前compaction没有负载过重，可以切换到新的memtable并触发旧的进行compaction
      assert(versions_->PrevLogNumber() == 0);
      uint64_t new_log_number = versions_->NewFileNumber();
      WritableFile* lfile = nullptr;
      s = env_->NewWritableFile(LogFileName(dbname_, new_log_number), &lfile); //生成新的log文件
      if (!s.ok()) {
        // Avoid chewing through file number space in a tight loop.
        versions_->ReuseFileNumber(new_log_number);
        break;
      }
      delete log_; //删除旧的log对象分配新的
      delete logfile_;
      logfile_ = lfile;
      logfile_number_ = new_log_number;
      log_ = new log::Writer(lfile);
      imm_ = mem_; //切换memtable到Imuable memtable
      has_imm_.store(true, std::memory_order_release);
      mem_ = new MemTable(internal_comparator_);
      mem_->Ref();
      force = false;  // Do not force another compaction if have room
      MaybeScheduleCompaction(); //如果需要进行compaction,后台执行
    }
  }
  return s;
}

bool DBImpl::GetProperty(const Slice& property, std::string* value) {
  value->clear();

  MutexLock l(&mutex_);
  Slice in = property;
  Slice prefix("leveldb.");
  if (!in.starts_with(prefix)) return false;
  in.remove_prefix(prefix.size());

  if (in.starts_with("num-files-at-level")) {
    in.remove_prefix(strlen("num-files-at-level"));
    uint64_t level;
    bool ok = ConsumeDecimalNumber(&in, &level) && in.empty();
    if (!ok || level >= config::kNumLevels) {
      return false;
    } else {
      char buf[100];
      snprintf(buf, sizeof(buf), "%d",
               versions_->NumLevelFiles(static_cast<int>(level)));
      *value = buf;
      return true;
    }
  } else if (in == "stats") {
    char buf[200];
    snprintf(buf, sizeof(buf),
             "                               Compactions\n"
             "Level  Files Size(MB) Time(sec) Read(MB) Write(MB)\n"
             "--------------------------------------------------\n");
    value->append(buf);
    for (int level = 0; level < config::kNumLevels; level++) {
      int files = versions_->NumLevelFiles(level);
      if (stats_[level].micros > 0 || files > 0) {
        snprintf(buf, sizeof(buf), "%3d %8d %8.0f %9.0f %8.0f %9.0f\n", level,
                 files, versions_->NumLevelBytes(level) / 1048576.0,
                 stats_[level].micros / 1e6,
                 stats_[level].bytes_read / 1048576.0,
                 stats_[level].bytes_written / 1048576.0);
        value->append(buf);
      }
    }
    return true;
  } else if (in == "sstables") {
    *value = versions_->current()->DebugString();
    return true;
  } else if (in == "approximate-memory-usage") {
    size_t total_usage = options_.block_cache->TotalCharge();
    if (mem_) {
      total_usage += mem_->ApproximateMemoryUsage();
    }
    if (imm_) {
      total_usage += imm_->ApproximateMemoryUsage();
    }
    char buf[50];
    snprintf(buf, sizeof(buf), "%llu",
             static_cast<unsigned long long>(total_usage));
    value->append(buf);
    return true;
  }

  return false;
}

void DBImpl::GetApproximateSizes(const Range* range, int n, uint64_t* sizes) {
  // TODO(opt): better implementation
  MutexLock l(&mutex_);
  Version* v = versions_->current();
  v->Ref();

  for (int i = 0; i < n; i++) {
    // Convert user_key into a corresponding internal key.
    InternalKey k1(range[i].start, kMaxSequenceNumber, kValueTypeForSeek);
    InternalKey k2(range[i].limit, kMaxSequenceNumber, kValueTypeForSeek);
    uint64_t start = versions_->ApproximateOffsetOf(v, k1);
    uint64_t limit = versions_->ApproximateOffsetOf(v, k2);
    sizes[i] = (limit >= start ? limit - start : 0);
  }

  v->Unref();
}

// Default implementations of convenience methods that subclasses of DB
// can call if they wish
Status DB::Put(const WriteOptions& opt, const Slice& key, const Slice& value) {
  WriteBatch batch; //leveldb中不管单个插入还是多个插入都是以WriteBatch的方式进行的
  batch.Put(key, value);
  return Write(opt, &batch);
}

Status DB::Delete(const WriteOptions& opt, const Slice& key) {
  WriteBatch batch;
  batch.Delete(key);
  return Write(opt, &batch);
}

DB::~DB() = default;

Status DB::Open(const Options& options, const std::string& dbname, DB** dbptr) {
  *dbptr = nullptr;

  DBImpl* impl = new DBImpl(options, dbname);
  impl->mutex_.Lock();
  VersionEdit edit;
  // Recover handles create_if_missing, error_if_exists
  bool save_manifest = false;
  Status s = impl->Recover(&edit, &save_manifest);
  if (s.ok() && impl->mem_ == nullptr) {
    // Create new log and a corresponding memtable.
    uint64_t new_log_number = impl->versions_->NewFileNumber();
    WritableFile* lfile;
    s = options.env->NewWritableFile(LogFileName(dbname, new_log_number),
                                     &lfile);
    uint64_t new_log_number_hot = impl->versions_->NewFileNumber();
    WritableFile* lfile_hot;
    s = options.env->NewWritableFile(LogFileName(dbname, new_log_number_hot),
                                     &lfile_hot);
    if (s.ok()) {
      edit.SetLogNumber(new_log_number);
      impl->logfile_ = lfile;
      impl->logfile_number_ = new_log_number;
      impl->log_ = new log::Writer(lfile);
      impl->log_hot_ = new log::Writer(lfile_hot);
      impl->mem_ = new MemTable(impl->internal_comparator_);
      impl->mem_->Ref();
      impl->mem_hot_ = new Skiplist_();

      // 调试代码
      impl->os1.open("/home/jingmu/print1.txt", std::ios::app);
      //impl->os2.open("/home/jingmu/print2.txt", std::ios::app);
      //impl->os3.open("/home/jingmu/print3.txt", std::ios::app);
      //impl->os4.open("/home/jingmu/print4.txt", std::ios::app);
      //impl->os5.open("/home/jingmu/print5.txt", std::ios::app);
      //impl->os6.open("/home/jingmu/print6.txt", std::ios::app);
      //impl->os7.open("/home/jingmu/print7.txt", std::ios::app);
    }
  }
  if (s.ok() && save_manifest) {
    edit.SetPrevLogNumber(0);  // No older logs needed after recovery.
    edit.SetLogNumber(impl->logfile_number_);
    s = impl->versions_->LogAndApply(&edit, &impl->mutex_);
  }
  if (s.ok()) {
    impl->DeleteObsoleteFiles();
    impl->MaybeScheduleCompaction();
  }
  impl->mutex_.Unlock();
  if (s.ok()) {
    assert(impl->mem_ != nullptr);
    *dbptr = impl;
  } else {
    delete impl;
  }
  return s;
}

Snapshot::~Snapshot() = default;

Status DestroyDB(const std::string& dbname, const Options& options) {
  Env* env = options.env;
  std::vector<std::string> filenames;
  Status result = env->GetChildren(dbname, &filenames);
  if (!result.ok()) {
    // Ignore error in case directory does not exist
    return Status::OK();
  }

  FileLock* lock;
  const std::string lockname = LockFileName(dbname);
  result = env->LockFile(lockname, &lock);
  if (result.ok()) {
    uint64_t number;
    FileType type;
    for (size_t i = 0; i < filenames.size(); i++) {
      if (ParseFileName(filenames[i], &number, &type) &&
          type != kDBLockFile) {  // Lock file will be deleted at end
        Status del = env->DeleteFile(dbname + "/" + filenames[i]);
        if (result.ok() && !del.ok()) {
          result = del;
        }
      }
    }
    env->UnlockFile(lock);  // Ignore error since state is already gone
    env->DeleteFile(lockname);
    env->DeleteDir(dbname);  // Ignore error in case dir contains other files
  }
  return result;
}

}  // namespace leveldb
