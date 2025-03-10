/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 */

#include "slot_migrate.h"

#include <memory>
#include <utility>

#include "db_util.h"
#include "event_util.h"
#include "fmt/format.h"
#include "io_util.h"
#include "storage/batch_extractor.h"
#include "thread_util.h"
#include "time_util.h"
#include "types/redis_stream_base.h"

const char *errFailedToSendCommands = "failed to send commands to restore a key";
const char *errMigrationTaskCanceled = "key migration stopped due to a task cancellation";
const char *errFailedToSetImportStatus = "failed to set import status on destination node";

static std::map<RedisType, std::string> type_to_cmd = {
    {kRedisString, "set"}, {kRedisList, "rpush"},    {kRedisHash, "hmset"},      {kRedisSet, "sadd"},
    {kRedisZSet, "zadd"},  {kRedisBitmap, "setbit"}, {kRedisSortedint, "siadd"}, {kRedisStream, "xadd"},
};

SlotMigrator::SlotMigrator(Server *svr, int max_migration_speed, int max_pipeline_size, int seq_gap_limit)
    : Database(svr->storage_, kDefaultNamespace), svr_(svr) {
  // Let metadata_cf_handle_ be nullptr, and get them in real time to avoid accessing invalid pointer,
  // because metadata_cf_handle_ and db_ will be destroyed if DB is reopened.
  // [Situation]:
  // 1. Start an empty slave server.
  // 2. Connect to master which has amounted of data, and trigger full synchronization.
  // 3. After replication, change slave to master and start slot migrate.
  // 4. It will occur segment fault when using metadata_cf_handle_ to create iterator of rocksdb.
  // [Reason]:
  // After full synchronization, DB will be reopened, db_ and metadata_cf_handle_ will be released.
  // Then, if we create rocksdb iterator with metadata_cf_handle_, it will go wrong.
  // [Solution]:
  // db_ and metadata_cf_handle_ will be replaced by storage_->GetDB() and storage_->GetCFHandle("metadata")
  // in all functions used in migration process.
  // [Note]:
  // This problem may exist in all functions of Database called in slot migration process.
  metadata_cf_handle_ = nullptr;

  if (max_migration_speed >= 0) {
    max_migration_speed_ = max_migration_speed;
  }
  if (max_pipeline_size > 0) {
    max_pipeline_size_ = max_pipeline_size;
  }
  if (seq_gap_limit > 0) {
    seq_gap_limit_ = seq_gap_limit;
  }

  if (svr->IsSlave()) {
    SetStopMigrationFlag(true);
  }
}

Status SlotMigrator::PerformSlotMigration(const std::string &node_id, std::string &dst_ip, int dst_port, int slot_id,
                                          int speed, int pipeline_size, int seq_gap) {
  // Only one slot migration job at the same time
  int16_t no_slot = -1;
  if (!migrating_slot_.compare_exchange_strong(no_slot, static_cast<int16_t>(slot_id))) {
    return {Status::NotOK, "There is already a migrating slot"};
  }

  if (forbidden_slot_ == slot_id) {
    // Have to release migrate slot set above
    migrating_slot_ = -1;
    return {Status::NotOK, "Can't migrate slot which has been migrated"};
  }

  migration_state_ = MigrationState::kStarted;

  if (speed <= 0) {
    speed = 0;
  }

  if (pipeline_size <= 0) {
    pipeline_size = kDefaultMaxPipelineSize;
  }

  if (seq_gap <= 0) {
    seq_gap = kDefaultSequenceGapLimit;
  }

  dst_node_ = node_id;

  // Create migration job
  auto job = std::make_unique<SlotMigrationJob>(slot_id, dst_ip, dst_port, speed, pipeline_size, seq_gap);
  {
    std::lock_guard<std::mutex> guard(job_mutex_);
    migration_job_ = std::move(job);
    job_cv_.notify_one();
  }

  LOG(INFO) << "[migrate] Start migrating slot " << slot_id << " to " << dst_ip << ":" << dst_port;

  return Status::OK();
}

SlotMigrator::~SlotMigrator() {
  if (thread_state_ == ThreadState::Running) {
    stop_migration_ = true;
    thread_state_ = ThreadState::Terminated;
    job_cv_.notify_all();
    if (auto s = Util::ThreadJoin(t_); !s) {
      LOG(WARNING) << "Slot migrating thread operation failed: " << s.Msg();
    }
  }
}

Status SlotMigrator::CreateMigrationThread() {
  t_ = GET_OR_RET(Util::CreateThread("slot-migrate", [this] {
    thread_state_ = ThreadState::Running;
    this->Loop();
  }));

  return Status::OK();
}

void SlotMigrator::Loop() {
  while (true) {
    std::unique_lock<std::mutex> ul(job_mutex_);
    while (!IsTerminated() && !migration_job_) {
      job_cv_.wait(ul);
    }
    ul.unlock();

    if (IsTerminated()) {
      Clean();
      return;
    }

    LOG(INFO) << "[migrate] Migrating slot: " << migration_job_->slot_id_ << ", dst_ip: " << migration_job_->dst_ip_
              << ", dst_port: " << migration_job_->dst_port_ << ", max_speed: " << migration_job_->max_speed_
              << ", max_pipeline_size: " << migration_job_->max_pipeline_size_;

    dst_ip_ = migration_job_->dst_ip_;
    dst_port_ = migration_job_->dst_port_;
    max_migration_speed_ = migration_job_->max_speed_;
    max_pipeline_size_ = migration_job_->max_pipeline_size_;
    seq_gap_limit_ = migration_job_->seq_gap_limit_;

    RunMigrationProcess();
  }
}

void SlotMigrator::RunMigrationProcess() {
  current_stage_ = SlotMigrationStage::kStart;

  while (true) {
    if (IsTerminated()) {
      LOG(WARNING) << "[migrate] Will stop state machine, because the thread was terminated";
      Clean();
      return;
    }

    switch (current_stage_) {
      case SlotMigrationStage::kStart: {
        auto s = StartMigration();
        if (s.IsOK()) {
          LOG(INFO) << "[migrate] Succeed to start migrating slot " << migrating_slot_;
          current_stage_ = SlotMigrationStage::kSnapshot;
        } else {
          LOG(ERROR) << "[migrate] Failed to start migrating slot " << migrating_slot_ << ". Error: " << s.Msg();
          current_stage_ = SlotMigrationStage::kFailed;
        }
        break;
      }
      case SlotMigrationStage::kSnapshot: {
        auto s = SendSnapshot();
        if (s.IsOK()) {
          current_stage_ = SlotMigrationStage::kWAL;
        } else {
          LOG(ERROR) << "[migrate] Failed to send snapshot of slot " << migrating_slot_ << ". Error: " << s.Msg();
          current_stage_ = SlotMigrationStage::kFailed;
        }
        break;
      }
      case SlotMigrationStage::kWAL: {
        auto s = SyncWAL();
        if (s.IsOK()) {
          LOG(INFO) << "[migrate] Succeed to sync from WAL for a slot " << migrating_slot_;
          current_stage_ = SlotMigrationStage::kSuccess;
        } else {
          LOG(ERROR) << "[migrate] Failed to sync from WAL for a slot " << migrating_slot_ << ". Error: " << s.Msg();
          current_stage_ = SlotMigrationStage::kFailed;
        }
        break;
      }
      case SlotMigrationStage::kSuccess: {
        auto s = FinishSuccessfulMigration();
        if (s.IsOK()) {
          LOG(INFO) << "[migrate] Succeed to migrate slot " << migrating_slot_;
          current_stage_ = SlotMigrationStage::kClean;
          migration_state_ = MigrationState::kSuccess;
        } else {
          LOG(ERROR) << "[migrate] Failed to finish a successful migration of slot " << migrating_slot_
                     << ". Error: " << s.Msg();
          current_stage_ = SlotMigrationStage::kFailed;
        }
        break;
      }
      case SlotMigrationStage::kFailed: {
        auto s = FinishFailedMigration();
        if (!s.IsOK()) {
          LOG(ERROR) << "[migrate] Failed to finish a failed migration of slot " << migrating_slot_
                     << ". Error: " << s.Msg();
        }
        LOG(INFO) << "[migrate] Failed to migrate a slot" << migrating_slot_;
        migration_state_ = MigrationState::kFailed;
        current_stage_ = SlotMigrationStage::kClean;
        break;
      }
      case SlotMigrationStage::kClean: {
        Clean();
        return;
      }
      default:
        LOG(ERROR) << "[migrate] Unexpected state for the state machine: " << static_cast<int>(current_stage_);
        Clean();
        return;
    }
  }
}

Status SlotMigrator::StartMigration() {
  // Get snapshot and sequence
  slot_snapshot_ = storage_->GetDB()->GetSnapshot();
  if (!slot_snapshot_) {
    return {Status::NotOK, "failed to create snapshot"};
  }

  wal_begin_seq_ = slot_snapshot_->GetSequenceNumber();
  last_send_time_ = 0;

  // Connect to the destination node
  auto result = Util::SockConnect(dst_ip_, dst_port_);
  if (!result.IsOK()) {
    return {Status::NotOK, fmt::format("failed to connect to the destination node: {}", result.Msg())};
  }

  dst_fd_.Reset(*result);

  // Auth first
  std::string pass = svr_->GetConfig()->requirepass;
  if (!pass.empty()) {
    auto s = AuthOnDstNode(*dst_fd_, pass);
    if (!s.IsOK()) {
      return s.Prefixed("failed to authenticate on destination node");
    }
  }

  // Set destination node import status to START
  auto s = SetImportStatusOnDstNode(*dst_fd_, kImportStart);
  if (!s.IsOK()) {
    return s.Prefixed(errFailedToSetImportStatus);
  }

  LOG(INFO) << "[migrate] Start migrating slot " << migrating_slot_ << ", connect destination fd " << *dst_fd_;

  return Status::OK();
}

Status SlotMigrator::SendSnapshot() {
  uint64_t migrated_key_cnt = 0;
  uint64_t expired_key_cnt = 0;
  uint64_t empty_key_cnt = 0;
  std::string restore_cmds;
  int16_t slot = migrating_slot_;

  LOG(INFO) << "[migrate] Start migrating snapshot of slot " << slot;

  rocksdb::ReadOptions read_options;
  read_options.snapshot = slot_snapshot_;
  storage_->SetReadOptions(read_options);
  rocksdb::ColumnFamilyHandle *cf_handle = storage_->GetCFHandle(Engine::kMetadataColumnFamilyName);
  auto iter = DBUtil::UniqueIterator(storage_->GetDB()->NewIterator(read_options, cf_handle));

  // Construct key prefix to iterate the keys belong to the target slot
  std::string prefix;
  ComposeSlotKeyPrefix(namespace_, slot, &prefix);
  LOG(INFO) << "[migrate] Iterate keys of slot, key's prefix: " << prefix;

  // Seek to the beginning of keys start with 'prefix' and iterate all these keys
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    // The migrating task has to be stopped, if server role is changed from master to slave
    // or flush command (flushdb or flushall) is executed
    if (stop_migration_) {
      return {Status::NotOK, errMigrationTaskCanceled};
    }

    // Iteration is out of range
    if (!iter->key().starts_with(prefix)) {
      break;
    }

    // Get user key
    std::string ns, user_key;
    ExtractNamespaceKey(iter->key(), &ns, &user_key, true);

    // Add key's constructed commands to restore_cmds, send pipeline or not according to task's max_pipeline_size
    auto result = MigrateOneKey(user_key, iter->value(), &restore_cmds);
    if (!result.IsOK()) {
      return {Status::NotOK, fmt::format("failed to migrate a key {}: {}", user_key, result.Msg())};
    }

    if (*result == KeyMigrationResult::kMigrated) {
      LOG(INFO) << "[migrate] The key " << user_key << " successfully migrated";
      migrated_key_cnt++;
    } else if (*result == KeyMigrationResult::kExpired) {
      LOG(INFO) << "[migrate] The key " << user_key << " is expired";
      expired_key_cnt++;
    } else if (*result == KeyMigrationResult::kUnderlyingStructEmpty) {
      LOG(INFO) << "[migrate] The key " << user_key << " has no elements";
      empty_key_cnt++;
    } else {
      LOG(ERROR) << "[migrate] Migrated a key " << user_key << " with unexpected result: " << static_cast<int>(*result);
      return {Status::NotOK};
    }
  }

  // It's necessary to send commands that are still in the pipeline since the final pipeline may not be sent
  // while iterating keys because its size could be less than max_pipeline_size_
  auto s = SendCmdsPipelineIfNeed(&restore_cmds, true);
  if (!s.IsOK()) {
    return s.Prefixed(errFailedToSendCommands);
  }

  LOG(INFO) << "[migrate] Succeed to migrate slot snapshot, slot: " << slot << ", Migrated keys: " << migrated_key_cnt
            << ", Expired keys: " << expired_key_cnt << ", Emtpy keys: " << empty_key_cnt;

  return Status::OK();
}

Status SlotMigrator::SyncWAL() {
  // Send incremental data from WAL circularly until new increment less than a certain amount
  auto s = SyncWALBeforeForbiddingSlot();
  if (!s.IsOK()) {
    return s.Prefixed("failed to sync WAL before forbidding a slot");
  }

  SetForbiddenSlot(migrating_slot_);

  // Send last incremental data
  s = SyncWALAfterForbiddingSlot();
  if (!s.IsOK()) {
    return s.Prefixed("failed to sync WAL after forbidding a slot");
  }

  return Status::OK();
}

Status SlotMigrator::FinishSuccessfulMigration() {
  if (stop_migration_) {
    return {Status::NotOK, errMigrationTaskCanceled};
  }

  // Set import status on the destination node to SUCCESS
  auto s = SetImportStatusOnDstNode(*dst_fd_, kImportSuccess);
  if (!s.IsOK()) {
    return s.Prefixed(errFailedToSetImportStatus);
  }

  std::string dst_ip_port = dst_ip_ + ":" + std::to_string(dst_port_);
  s = svr_->cluster_->SetSlotMigrated(migrating_slot_, dst_ip_port);
  if (!s.IsOK()) {
    return s.Prefixed(fmt::format("failed to set slot {} as migrated to {}", migrating_slot_, dst_ip_port));
  }

  migrate_failed_slot_ = -1;

  return Status::OK();
}

Status SlotMigrator::FinishFailedMigration() {
  // Stop slot will forbid writing
  migrate_failed_slot_ = migrating_slot_;
  forbidden_slot_ = -1;

  // Set import status on the destination node to FAILED
  auto s = SetImportStatusOnDstNode(*dst_fd_, kImportFailed);
  if (!s.IsOK()) {
    return s.Prefixed(errFailedToSetImportStatus);
  }

  return Status::OK();
}

void SlotMigrator::Clean() {
  LOG(INFO) << "[migrate] Clean resources of migrating slot " << migrating_slot_;
  if (slot_snapshot_) {
    storage_->GetDB()->ReleaseSnapshot(slot_snapshot_);
    slot_snapshot_ = nullptr;
  }

  current_stage_ = SlotMigrationStage::kNone;
  current_pipeline_size_ = 0;
  wal_begin_seq_ = 0;
  std::lock_guard<std::mutex> guard(job_mutex_);
  migration_job_.reset();
  dst_fd_.Reset();
  migrating_slot_ = -1;
  SetStopMigrationFlag(false);
}

Status SlotMigrator::AuthOnDstNode(int sock_fd, const std::string &password) {
  std::string cmd = Redis::MultiBulkString({"auth", password}, false);
  auto s = Util::SockSend(sock_fd, cmd);
  if (!s.IsOK()) {
    return s.Prefixed("failed to send AUTH command");
  }

  s = CheckSingleResponse(sock_fd);
  if (!s.IsOK()) {
    return s.Prefixed("failed to check the response of AUTH command");
  }

  return Status::OK();
}

Status SlotMigrator::SetImportStatusOnDstNode(int sock_fd, int status) {
  if (sock_fd <= 0) return {Status::NotOK, "invalid socket descriptor"};

  std::string cmd =
      Redis::MultiBulkString({"cluster", "import", std::to_string(migrating_slot_), std::to_string(status)});
  auto s = Util::SockSend(sock_fd, cmd);
  if (!s.IsOK()) {
    return s.Prefixed("failed to send command to the destination node");
  }

  s = CheckSingleResponse(sock_fd);
  if (!s.IsOK()) {
    return s.Prefixed("failed to check the response from the destination node");
  }

  return Status::OK();
}

Status SlotMigrator::CheckSingleResponse(int sock_fd) { return CheckMultipleResponses(sock_fd, 1); }

// Commands  |  Response            |  Instance
// ++++++++++++++++++++++++++++++++++++++++
// set          Redis::Integer         :1/r/n
// hset         Redis::SimpleString    +OK/r/n
// sadd         Redis::Integer
// zadd         Redis::Integer
// siadd        Redis::Integer
// setbit       Redis::Integer
// expire       Redis::Integer
// lpush        Redis::Integer
// rpush        Redis::Integer
// ltrim        Redis::SimpleString    -Err\r\n
// linsert      Redis::Integer
// lset         Redis::SimpleString
// hdel         Redis::Integer
// srem         Redis::Integer
// zrem         Redis::Integer
// lpop         Redis::NilString       $-1\r\n
//          or  Redis::BulkString      $1\r\n1\r\n
// rpop         Redis::NilString
//          or  Redis::BulkString
// lrem         Redis::Integer
// sirem        Redis::Integer
// del          Redis::Integer
// xadd         Redis::BulkString
Status SlotMigrator::CheckMultipleResponses(int sock_fd, int total) {
  if (sock_fd < 0 || total <= 0) {
    return {Status::NotOK, fmt::format("invalid arguments: sock_fd={}, count={}", sock_fd, total)};
  }

  // Set socket receive timeout first
  struct timeval tv;
  tv.tv_sec = 1;
  tv.tv_usec = 0;
  setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  // Start checking response
  size_t bulk_len = 0;
  int cnt = 0;
  parser_state_ = ParserState::ArrayLen;
  UniqueEvbuf evbuf;
  while (true) {
    // Read response data from socket buffer to the event buffer
    if (evbuffer_read(evbuf.get(), sock_fd, -1) <= 0) {
      return {Status::NotOK, fmt::format("failed to read response: {}", strerror(errno))};
    }

    // Parse response data in event buffer
    bool run = true;
    while (run) {
      switch (parser_state_) {
        // Handle single string response
        case ParserState::ArrayLen: {
          UniqueEvbufReadln line(evbuf.get(), EVBUFFER_EOL_CRLF_STRICT);
          if (!line) {
            LOG(INFO) << "[migrate] Event buffer is empty, read socket again";
            run = false;
            break;
          }

          if (line[0] == '-') {
            return {Status::NotOK, fmt::format("got invalid response of length {}: {}", line.length, line.get())};
          } else if (line[0] == '$') {
            auto parse_result = ParseInt<uint64_t>(std::string(line.get() + 1, line.length - 1), 10);
            if (!parse_result) {
              return {Status::NotOK, "protocol error: expected integer value"};
            }

            bulk_len = *parse_result;
            parser_state_ = bulk_len > 0 ? ParserState::BulkData : ParserState::OneRspEnd;
          } else if (line[0] == '+' || line[0] == ':') {
            parser_state_ = ParserState::OneRspEnd;
          } else {
            return {Status::NotOK, fmt::format("got unexpected response of length {}: {}", line.length, line.get())};
          }

          break;
        }
        // Handle bulk string response
        case ParserState::BulkData: {
          if (evbuffer_get_length(evbuf.get()) < bulk_len + 2) {
            LOG(INFO) << "[migrate] Bulk data in event buffer is not complete, read socket again";
            run = false;
            break;
          }
          // TODO(chrisZMF): Check tail '\r\n'
          evbuffer_drain(evbuf.get(), bulk_len + 2);
          bulk_len = 0;
          parser_state_ = ParserState::OneRspEnd;
          break;
        }
        case ParserState::OneRspEnd: {
          cnt++;
          if (cnt >= total) {
            return Status::OK();
          }

          parser_state_ = ParserState::ArrayLen;
          break;
        }
        default:
          break;
      }
    }
  }
}

StatusOr<KeyMigrationResult> SlotMigrator::MigrateOneKey(const rocksdb::Slice &key,
                                                         const rocksdb::Slice &encoded_metadata,
                                                         std::string *restore_cmds) {
  std::string bytes = encoded_metadata.ToString();
  Metadata metadata(kRedisNone, false);
  metadata.Decode(bytes);

  if (metadata.Type() != kRedisString && metadata.Type() != kRedisStream && metadata.size == 0) {
    return KeyMigrationResult::kUnderlyingStructEmpty;
  }

  if (metadata.Expired()) {
    return KeyMigrationResult::kExpired;
  }

  // Construct command according to type of the key
  switch (metadata.Type()) {
    case kRedisString: {
      auto s = MigrateSimpleKey(key, metadata, bytes, restore_cmds);
      if (!s.IsOK()) {
        return s.Prefixed("failed to migrate simple key");
      }
      break;
    }
    case kRedisList:
    case kRedisZSet:
    case kRedisBitmap:
    case kRedisHash:
    case kRedisSet:
    case kRedisSortedint: {
      auto s = MigrateComplexKey(key, metadata, restore_cmds);
      if (!s.IsOK()) {
        return s.Prefixed("failed to migrate complex key");
      }
      break;
    }
    case kRedisStream: {
      StreamMetadata stream_md(false);
      stream_md.Decode(bytes);

      auto s = MigrateStream(key, stream_md, restore_cmds);
      if (!s.IsOK()) {
        return s.Prefixed("failed to migrate stream key");
      }
      break;
    }
    default:
      break;
  }

  return KeyMigrationResult::kMigrated;
}

Status SlotMigrator::MigrateSimpleKey(const rocksdb::Slice &key, const Metadata &metadata, const std::string &bytes,
                                      std::string *restore_cmds) {
  std::vector<std::string> command = {"SET", key.ToString(), bytes.substr(Metadata::GetOffsetAfterExpire(bytes[0]))};
  if (metadata.expire > 0) {
    command.emplace_back("PXAT");
    command.emplace_back(std::to_string(metadata.expire));
  }
  *restore_cmds += Redis::MultiBulkString(command, false);
  current_pipeline_size_++;

  // Check whether pipeline needs to be sent
  // TODO(chrisZMF): Resend data if failed to send data
  auto s = SendCmdsPipelineIfNeed(restore_cmds, false);
  if (!s.IsOK()) {
    return s.Prefixed(errFailedToSendCommands);
  }

  return Status::OK();
}

Status SlotMigrator::MigrateComplexKey(const rocksdb::Slice &key, const Metadata &metadata, std::string *restore_cmds) {
  std::string cmd;
  cmd = type_to_cmd[metadata.Type()];

  std::vector<std::string> user_cmd = {cmd, key.ToString()};
  rocksdb::ReadOptions read_options;
  read_options.snapshot = slot_snapshot_;
  storage_->SetReadOptions(read_options);
  // Should use th raw db iterator to avoid reading uncommitted writes in transaction mode
  auto iter = DBUtil::UniqueIterator(storage_->GetDB()->NewIterator(read_options));

  // Construct key prefix to iterate values of the complex type user key
  std::string slot_key, prefix_subkey;
  AppendNamespacePrefix(key, &slot_key);
  InternalKey(slot_key, "", metadata.version, true).Encode(&prefix_subkey);
  int item_count = 0;

  for (iter->Seek(prefix_subkey); iter->Valid(); iter->Next()) {
    if (stop_migration_) {
      return {Status::NotOK, errMigrationTaskCanceled};
    }

    if (!iter->key().starts_with(prefix_subkey)) {
      break;
    }

    // Parse values of the complex key
    // InternalKey is adopted to get complex key's value from the formatted key return by iterator of rocksdb
    InternalKey inkey(iter->key(), true);
    switch (metadata.Type()) {
      case kRedisSet: {
        user_cmd.emplace_back(inkey.GetSubKey().ToString());
        break;
      }
      case kRedisSortedint: {
        auto id = DecodeFixed64(inkey.GetSubKey().ToString().data());
        user_cmd.emplace_back(std::to_string(id));
        break;
      }
      case kRedisZSet: {
        auto score = DecodeDouble(iter->value().ToString().data());
        user_cmd.emplace_back(Util::Float2String(score));
        user_cmd.emplace_back(inkey.GetSubKey().ToString());
        break;
      }
      case kRedisBitmap: {
        auto s = MigrateBitmapKey(inkey, &iter, &user_cmd, restore_cmds);
        if (!s.IsOK()) {
          return s.Prefixed("failed to migrate bitmap key");
        }
        break;
      }
      case kRedisHash: {
        user_cmd.emplace_back(inkey.GetSubKey().ToString());
        user_cmd.emplace_back(iter->value().ToString());
        break;
      }
      case kRedisList: {
        user_cmd.emplace_back(iter->value().ToString());
        break;
      }
      default:
        break;
    }

    // Check item count
    // Exclude bitmap because it does not have hmset-like command
    if (metadata.Type() != kRedisBitmap) {
      item_count++;
      if (item_count >= kMaxItemsInCommand) {
        *restore_cmds += Redis::MultiBulkString(user_cmd, false);
        current_pipeline_size_++;
        item_count = 0;
        // Have to clear saved items
        user_cmd.erase(user_cmd.begin() + 2, user_cmd.end());

        // Send commands if the pipeline contains enough of them
        auto s = SendCmdsPipelineIfNeed(restore_cmds, false);
        if (!s.IsOK()) {
          return s.Prefixed(errFailedToSendCommands);
        }
      }
    }
  }

  // Have to check the item count of the last command list
  if (item_count % kMaxItemsInCommand != 0) {
    *restore_cmds += Redis::MultiBulkString(user_cmd, false);
    current_pipeline_size_++;
  }

  // Add TTL for complex key
  if (metadata.expire > 0) {
    *restore_cmds += Redis::MultiBulkString({"PEXPIREAT", key.ToString(), std::to_string(metadata.expire)}, false);
    current_pipeline_size_++;
  }

  // Send commands if the pipeline contains enough of them
  auto s = SendCmdsPipelineIfNeed(restore_cmds, false);
  if (!s.IsOK()) {
    return s.Prefixed(errFailedToSendCommands);
  }

  return Status::OK();
}

Status SlotMigrator::MigrateStream(const Slice &key, const StreamMetadata &metadata, std::string *restore_cmds) {
  rocksdb::ReadOptions read_options;
  read_options.snapshot = slot_snapshot_;
  storage_->SetReadOptions(read_options);
  // Should use th raw db iterator to avoid reading uncommitted writes in transaction mode
  auto iter = DBUtil::UniqueIterator(
      storage_->GetDB()->NewIterator(read_options, storage_->GetCFHandle(Engine::kStreamColumnFamilyName)));

  std::string ns_key;
  AppendNamespacePrefix(key, &ns_key);
  // Construct key prefix to iterate values of the stream
  std::string prefix_key;
  InternalKey(ns_key, "", metadata.version, true).Encode(&prefix_key);

  std::vector<std::string> user_cmd = {type_to_cmd[metadata.Type()], key.ToString()};

  for (iter->Seek(prefix_key); iter->Valid(); iter->Next()) {
    if (stop_migration_) {
      return {Status::NotOK, errMigrationTaskCanceled};
    }

    if (!iter->key().starts_with(prefix_key)) {
      break;
    }

    auto s = WriteBatchExtractor::ExtractStreamAddCommand(true, iter->key(), iter->value(), &user_cmd);
    if (!s.IsOK()) {
      return s;
    }
    *restore_cmds += Redis::MultiBulkString(user_cmd, false);
    current_pipeline_size_++;

    user_cmd.erase(user_cmd.begin() + 2, user_cmd.end());

    s = SendCmdsPipelineIfNeed(restore_cmds, false);
    if (!s.IsOK()) {
      return s.Prefixed(errFailedToSendCommands);
    }
  }

  // commands like XTRIM and XDEL affect stream's metadata, but we use only XADD for a slot migration
  // XSETID is used to adjust stream's info on the destination node according to the current values on the source
  *restore_cmds += Redis::MultiBulkString(
      {"XSETID", key.ToString(), metadata.last_generated_id.ToString(), "ENTRIESADDED",
       std::to_string(metadata.entries_added), "MAXDELETEDID", metadata.max_deleted_entry_id.ToString()},
      false);
  current_pipeline_size_++;

  // Add TTL
  if (metadata.expire > 0) {
    *restore_cmds += Redis::MultiBulkString({"PEXPIREAT", key.ToString(), std::to_string(metadata.expire)}, false);
    current_pipeline_size_++;
  }

  auto s = SendCmdsPipelineIfNeed(restore_cmds, false);
  if (!s.IsOK()) {
    return s.Prefixed(errFailedToSendCommands);
  }

  return Status::OK();
}

Status SlotMigrator::MigrateBitmapKey(const InternalKey &inkey, std::unique_ptr<rocksdb::Iterator> *iter,
                                      std::vector<std::string> *user_cmd, std::string *restore_cmds) {
  std::string index_str = inkey.GetSubKey().ToString();
  std::string fragment = (*iter)->value().ToString();
  auto parse_result = ParseInt<int>(index_str, 10);
  if (!parse_result) {
    return {Status::RedisParseErr, "index is not a valid integer"};
  }

  uint32_t index = *parse_result;

  // Bitmap does not have hmset-like command
  // TODO(chrisZMF): Use hmset-like command for efficiency
  for (int byte_idx = 0; byte_idx < static_cast<int>(fragment.size()); byte_idx++) {
    if (fragment[byte_idx] & 0xff) {
      for (int bit_idx = 0; bit_idx < 8; bit_idx++) {
        if (fragment[byte_idx] & (1 << bit_idx)) {
          uint32_t offset = (index * 8) + (byte_idx * 8) + bit_idx;
          user_cmd->emplace_back(std::to_string(offset));
          user_cmd->emplace_back("1");
          *restore_cmds += Redis::MultiBulkString(*user_cmd, false);
          current_pipeline_size_++;
          user_cmd->erase(user_cmd->begin() + 2, user_cmd->end());
        }
      }

      auto s = SendCmdsPipelineIfNeed(restore_cmds, false);
      if (!s.IsOK()) {
        return s.Prefixed(errFailedToSendCommands);
      }
    }
  }

  return Status::OK();
}

Status SlotMigrator::SendCmdsPipelineIfNeed(std::string *commands, bool need) {
  if (stop_migration_) {
    return {Status::NotOK, errMigrationTaskCanceled};
  }

  // Check pipeline
  if (!need && current_pipeline_size_ < max_pipeline_size_) {
    return Status::OK();
  }

  if (current_pipeline_size_ == 0) {
    LOG(INFO) << "[migrate] No commands to send";
    return Status::OK();
  }

  ApplyMigrationSpeedLimit();

  auto s = Util::SockSend(*dst_fd_, *commands);
  if (!s.IsOK()) {
    return s.Prefixed("failed to write data to a socket");
  }

  last_send_time_ = Util::GetTimeStampUS();

  s = CheckMultipleResponses(*dst_fd_, current_pipeline_size_);
  if (!s.IsOK()) {
    return s.Prefixed("wrong response from the destination node");
  }

  // Clear commands and running pipeline
  commands->clear();
  current_pipeline_size_ = 0;

  return Status::OK();
}

void SlotMigrator::SetForbiddenSlot(int16_t slot) {
  LOG(INFO) << "[migrate] Setting forbidden slot " << slot;
  // Block server to set forbidden slot
  uint64_t during = Util::GetTimeStampUS();
  {
    auto exclusivity = svr_->WorkExclusivityGuard();
    forbidden_slot_ = slot;
  }
  during = Util::GetTimeStampUS() - during;
  LOG(INFO) << "[migrate] To set forbidden slot, server was blocked for " << during << "us";
}

void SlotMigrator::ReleaseForbiddenSlot() {
  LOG(INFO) << "[migrate] Release forbidden slot " << forbidden_slot_;
  forbidden_slot_ = -1;
}

void SlotMigrator::ApplyMigrationSpeedLimit() const {
  if (max_migration_speed_ > 0) {
    uint64_t current_time = Util::GetTimeStampUS();
    uint64_t per_request_time = 1000000 * max_pipeline_size_ / max_migration_speed_;
    if (per_request_time == 0) {
      per_request_time = 1;
    }
    if (last_send_time_ + per_request_time > current_time) {
      uint64_t during = last_send_time_ + per_request_time - current_time;
      LOG(INFO) << "[migrate] Sleep to limit migration speed for: " << during;
      std::this_thread::sleep_for(std::chrono::microseconds(during));
    }
  }
}

Status SlotMigrator::GenerateCmdsFromBatch(rocksdb::BatchResult *batch, std::string *commands) {
  // Iterate batch to get keys and construct commands for keys
  WriteBatchExtractor write_batch_extractor(storage_->IsSlotIdEncoded(), migrating_slot_, false);
  rocksdb::Status status = batch->writeBatchPtr->Iterate(&write_batch_extractor);
  if (!status.ok()) {
    LOG(ERROR) << "[migrate] Failed to parse write batch, Err: " << status.ToString();
    return {Status::NotOK};
  }

  // Get all constructed commands
  auto resp_commands = write_batch_extractor.GetRESPCommands();
  for (const auto &iter : *resp_commands) {
    for (const auto &it : iter.second) {
      *commands += it;
      current_pipeline_size_++;
    }
  }

  return Status::OK();
}

Status SlotMigrator::MigrateIncrementData(std::unique_ptr<rocksdb::TransactionLogIterator> *iter, uint64_t end_seq) {
  if (!(*iter) || !(*iter)->Valid()) {
    LOG(ERROR) << "[migrate] WAL iterator is invalid";
    return {Status::NotOK};
  }

  uint64_t next_seq = wal_begin_seq_ + 1;
  std::string commands;

  while (true) {
    if (stop_migration_) {
      LOG(ERROR) << "[migrate] Migration task end during migrating WAL data";
      return {Status::NotOK};
    }

    auto batch = (*iter)->GetBatch();
    if (batch.sequence != next_seq) {
      LOG(ERROR) << "[migrate] WAL iterator is discrete, some seq might be lost"
                 << ", expected sequence: " << next_seq << ", but got sequence: " << batch.sequence;
      return {Status::NotOK};
    }

    // Generate commands by iterating write batch
    auto s = GenerateCmdsFromBatch(&batch, &commands);
    if (!s.IsOK()) {
      LOG(ERROR) << "[migrate] Failed to generate commands from write batch";
      return {Status::NotOK};
    }

    // Check whether command pipeline should be sent
    s = SendCmdsPipelineIfNeed(&commands, false);
    if (!s.IsOK()) {
      LOG(ERROR) << "[migrate] Failed to send WAL commands pipeline";
      return {Status::NotOK};
    }

    next_seq = batch.sequence + batch.writeBatchPtr->Count();
    if (next_seq > end_seq) {
      LOG(INFO) << "[migrate] Migrate incremental data an epoch OK, seq from " << wal_begin_seq_ << ", to " << end_seq;
      break;
    }

    (*iter)->Next();
    if (!(*iter)->Valid()) {
      LOG(ERROR) << "[migrate] WAL iterator is invalid, expected end seq: " << end_seq << ", next seq: " << next_seq;
      return {Status::NotOK};
    }
  }

  // Send the left data of this epoch
  auto s = SendCmdsPipelineIfNeed(&commands, true);
  if (!s.IsOK()) {
    LOG(ERROR) << "[migrate] Failed to send WAL last commands in pipeline";
    return {Status::NotOK};
  }

  return Status::OK();
}

Status SlotMigrator::SyncWALBeforeForbiddingSlot() {
  uint32_t count = 0;

  while (count < kMaxLoopTimes) {
    uint64_t latest_seq = storage_->GetDB()->GetLatestSequenceNumber();
    uint64_t gap = latest_seq - wal_begin_seq_;
    if (gap <= static_cast<uint64_t>(seq_gap_limit_)) {
      LOG(INFO) << "[migrate] Incremental data sequence: " << gap << ", less than limit: " << seq_gap_limit_
                << ", go to set forbidden slot";
      break;
    }

    std::unique_ptr<rocksdb::TransactionLogIterator> iter = nullptr;
    auto s = storage_->GetWALIter(wal_begin_seq_ + 1, &iter);
    if (!s.IsOK()) {
      LOG(ERROR) << "[migrate] Failed to generate WAL iterator before setting forbidden slot"
                 << ", Err: " << s.Msg();
      return {Status::NotOK};
    }

    // Iterate wal and migrate data
    s = MigrateIncrementData(&iter, latest_seq);
    if (!s.IsOK()) {
      LOG(ERROR) << "[migrate] Failed to migrate WAL data before setting forbidden slot";
      return {Status::NotOK};
    }

    wal_begin_seq_ = latest_seq;
    count++;
  }

  LOG(INFO) << "[migrate] Succeed to migrate incremental data before setting forbidden slot, end epoch: " << count;
  return Status::OK();
}

Status SlotMigrator::SyncWALAfterForbiddingSlot() {
  uint64_t latest_seq = storage_->GetDB()->GetLatestSequenceNumber();

  // No incremental data
  if (latest_seq <= wal_begin_seq_) return Status::OK();

  // Get WAL iter
  std::unique_ptr<rocksdb::TransactionLogIterator> iter = nullptr;
  auto s = storage_->GetWALIter(wal_begin_seq_ + 1, &iter);
  if (!s.IsOK()) {
    LOG(ERROR) << "[migrate] Failed to generate WAL iterator after setting forbidden slot"
               << ", Err: " << s.Msg();
    return {Status::NotOK};
  }

  // Send incremental data
  s = MigrateIncrementData(&iter, latest_seq);
  if (!s.IsOK()) {
    LOG(ERROR) << "[migrate] Failed to migrate WAL data after setting forbidden slot";
    return {Status::NotOK};
  }

  return Status::OK();
}

void SlotMigrator::GetMigrationInfo(std::string *info) const {
  info->clear();
  if (migrating_slot_ < 0 && forbidden_slot_ < 0 && migrate_failed_slot_ < 0) {
    return;
  }

  int16_t slot = -1;
  std::string task_state;

  switch (migration_state_.load()) {
    case MigrationState::kNone:
      task_state = "none";
      break;
    case MigrationState::kStarted:
      task_state = "start";
      slot = migrating_slot_;
      break;
    case MigrationState::kSuccess:
      task_state = "success";
      slot = forbidden_slot_;
      break;
    case MigrationState::kFailed:
      task_state = "fail";
      slot = migrate_failed_slot_;
      break;
    default:
      break;
  }

  *info =
      fmt::format("migrating_slot: {}\r\ndestination_node: {}\r\nmigrating_state: {}\r\n", slot, dst_node_, task_state);
}
