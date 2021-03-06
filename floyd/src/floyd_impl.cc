// Copyright (c) 2015-present, Qihoo, Inc.  All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.

#include "floyd/src/floyd_impl.h"

#include <utility>
#include <vector>

#include "slash/include/env.h"

#include "slash/include/slash_string.h"
#include "floyd/src/floyd_context.h"
#include "floyd/src/floyd_apply.h"
#include "floyd/src/floyd_worker.h"
#include "floyd/src/raft_log.h"
#include "floyd/src/floyd_peer_thread.h"
#include "floyd/src/floyd_primary_thread.h"
#include "floyd/src/floyd_client_pool.h"
#include "floyd/src/logger.h"

namespace floyd {

FloydImpl::FloydImpl(const Options& options)
  : options_(options),
    db_(NULL),
    info_log_(NULL) {
}

FloydImpl::~FloydImpl() {
  // worker will use floyd, delete worker first
  delete worker_;
  delete worker_client_pool_;
  delete peer_client_pool_;
  delete primary_;
  delete apply_;
  for (auto& pt : peers_) {
    delete pt.second;
  }

  delete context_;
  delete db_;
  delete raft_log_;
  delete info_log_;
}

bool FloydImpl::IsSelf(const std::string& ip_port) {
  return (ip_port ==
    slash::IpPortString(options_.local_ip, options_.local_port));
}

bool FloydImpl::GetLeader(std::string *ip_port) {
  std::string ip;
  int port;
  context_->leader_node(&ip, &port);
  if (ip.empty() || port == 0) {
    return false;
  }
  *ip_port = slash::IpPortString(ip, port);
  return true;
}

// TODO (baotiao): this function is wrong
bool FloydImpl::GetLeader(std::string* ip, int* port) {
  context_->leader_node(ip, port);
  return (!ip->empty() && *port != 0);
}

bool FloydImpl::HasLeader() {
  return context_->HasLeader();
}

bool FloydImpl::GetAllNodes(std::vector<std::string>& nodes) {
  nodes = options_.members;
  return true;
}

void FloydImpl::set_log_level(const int log_level) {
  if (info_log_) {
    info_log_->set_log_level(log_level);
  }
}

Status FloydImpl::Init() {
  slash::CreatePath(options_.path);
  if (NewLogger(options_.path + "/LOG", &info_log_) != 0) {
    return Status::Corruption("Open LOG failed, ", strerror(errno));
  }

  // TODO (anan) set timeout and retry
  peer_client_pool_ = new ClientPool(info_log_);
  worker_client_pool_ = new ClientPool(info_log_);

  // Create DB
  rocksdb::Options options;
  options.create_if_missing = true;
  rocksdb::Status s = rocksdb::DB::Open(options, options_.path + "/db/", &db_);
  if (!s.ok()) {
    LOGV(ERROR_LEVEL, info_log_, "Open db failed! path: %s", options_.path.c_str());
    return Status::Corruption("Open DB failed, " + s.ToString());
  }

  // Recover Context
  raft_log_ = new RaftLog(options_.path + "/log/", info_log_);
  context_ = new FloydContext(options_, raft_log_, info_log_);
  context_->RecoverInit();

  // Create Apply threads
  apply_ = new FloydApply(context_, db_, raft_log_);

  // TODO(annan) peers and primary refer to each other
  // Create PrimaryThread before Peers
  primary_ = new FloydPrimary(context_, apply_);

  // Create peer threads
  for (auto iter = options_.members.begin();
      iter != options_.members.end(); iter++) {
    if (!IsSelf(*iter)) {
      Peer* pt = new Peer(*iter, context_, primary_, raft_log_, peer_client_pool_);
      peers_.insert(std::pair<std::string, Peer*>(*iter, pt));
    }
  }

  // Start peer thread
  int ret;
  for (auto& pt : peers_) {
    if ((ret = pt.second->StartThread()) != 0) {
      LOGV(ERROR_LEVEL, info_log_, "FloydImpl peer thread to %s failed to "
           " start, ret is %d", pt.first.c_str(), ret);
      return Status::Corruption("failed to start peer thread to " + pt.first);
    }
  }
  LOGV(INFO_LEVEL, info_log_, "Floyd start %d peer thread", peers_.size());

  // Start worker thread after Peers, because WorkerHandle will check peers
  worker_ = new FloydWorker(options_.local_port, 1000, this);
  if ((ret = worker_->Start()) != 0) {
    LOGV(ERROR_LEVEL, info_log_, "FloydImpl worker thread failed to start, ret is %d", ret);
    return Status::Corruption("failed to start worker, return " + std::to_string(ret));
  }

  // Set and Start PrimaryThread
  primary_->set_peers(&peers_);
  if ((ret = primary_->Start()) != 0) {
    LOGV(ERROR_LEVEL, info_log_, "FloydImpl primary thread failed to start, ret is %d", ret);
    return Status::Corruption("failed to start primary thread, return " + std::to_string(ret));
  }
  primary_->AddTask(kCheckElectLeader);

  // test only
  // options_.Dump();
  LOGV(INFO_LEVEL, info_log_, "Floyd started!\nOptions\n%s", options_.ToString().c_str());
  return Status::OK();
}

Status Floyd::Open(const Options& options, Floyd** floyd) {
  *floyd = NULL;
  Status s;
  FloydImpl *impl = new FloydImpl(options);
  s = impl->Init();
  if (s.ok()) {
    *floyd = impl;
  } else {
    delete impl;
  }
  return s;
}

Floyd::~Floyd() {
}

} // namespace floyd
