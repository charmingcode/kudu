// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "kudu/master/master.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <ostream>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "kudu/cfile/block_cache.h"
#include "kudu/common/wire_protocol.h"
#include "kudu/common/wire_protocol.pb.h"
#include "kudu/consensus/metadata.pb.h"
#include "kudu/fs/fs_manager.h"
#include "kudu/gutil/bind.h"
#include "kudu/gutil/bind_helpers.h"
#include "kudu/gutil/map-util.h"
#include "kudu/gutil/move.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/master/catalog_manager.h"
#include "kudu/master/master.pb.h"
#include "kudu/master/master.proxy.h"
#include "kudu/master/master_cert_authority.h"
#include "kudu/master/master_path_handlers.h"
#include "kudu/master/master_service.h"
#include "kudu/master/ts_manager.h"
#include "kudu/rpc/messenger.h"
#include "kudu/rpc/rpc_controller.h"
#include "kudu/rpc/service_if.h"
#include "kudu/security/token_signer.h"
#include "kudu/server/rpc_server.h"
#include "kudu/server/webserver.h"
#include "kudu/tserver/tablet_copy_service.h"
#include "kudu/tserver/tablet_service.h"
#include "kudu/util/flag_tags.h"
#include "kudu/util/maintenance_manager.h"
#include "kudu/util/monotime.h"
#include "kudu/util/net/net_util.h"
#include "kudu/util/net/sockaddr.h"
#include "kudu/util/status.h"
#include "kudu/util/threadpool.h"
#include "kudu/util/version_info.h"

DEFINE_int32(master_registration_rpc_timeout_ms, 1500,
             "Timeout for retrieving master registration over RPC.");
TAG_FLAG(master_registration_rpc_timeout_ms, experimental);

DEFINE_int64(tsk_rotation_seconds, 60 * 60 * 24 * 1,
             "Number of seconds between consecutive activations of newly "
             "generated TSKs (Token Signing Keys).");
TAG_FLAG(tsk_rotation_seconds, advanced);
TAG_FLAG(tsk_rotation_seconds, experimental);

DEFINE_int64(authn_token_validity_seconds, 60 * 60 * 24 * 7,
             "Period of time for which an issued authentication token is valid. "
             "It's not possible to renew a token, hence the token validity "
             "interval defines the longest possible lifetime of an external "
             "job which uses a token for authentication.");
TAG_FLAG(authn_token_validity_seconds, experimental);

using std::min;
using std::shared_ptr;
using std::string;
using std::vector;

using kudu::consensus::RaftPeerPB;
using kudu::rpc::ServiceIf;
using kudu::security::TokenSigner;
using kudu::tserver::ConsensusServiceImpl;
using kudu::tserver::TabletCopyServiceImpl;
using strings::Substitute;

namespace kudu {
namespace master {

Master::Master(const MasterOptions& opts)
  : KuduServer("Master", opts, "kudu.master"),
    state_(kStopped),
    ts_manager_(new TSManager()),
    catalog_manager_(new CatalogManager(this)),
    path_handlers_(new MasterPathHandlers(this)),
    opts_(opts),
    registration_initialized_(false),
    maintenance_manager_(new MaintenanceManager(MaintenanceManager::kDefaultOptions)) {
}

Master::~Master() {
  CHECK_NE(kRunning, state_);
}

string Master::ToString() const {
  if (state_ != kRunning) {
    return "Master (stopped)";
  }
  return strings::Substitute("Master@$0", first_rpc_address().ToString());
}

Status Master::Init() {
  CHECK_EQ(kStopped, state_);

  cfile::BlockCache::GetSingleton()->StartInstrumentation(metric_entity());

  RETURN_NOT_OK(ThreadPoolBuilder("init").set_max_threads(1).Build(&init_pool_));

  RETURN_NOT_OK(KuduServer::Init());

  if (web_server_) {
    RETURN_NOT_OK(path_handlers_->Register(web_server_.get()));
  }

  // The certificate authority object is initialized upon loading
  // CA private key and certificate from the system table when the server
  // becomes a leader.
  cert_authority_.reset(new MasterCertAuthority(fs_manager_->uuid()));

  // The TokenSigner loads its keys during catalog manager initialization.
  token_signer_.reset(new TokenSigner(
      FLAGS_authn_token_validity_seconds,
      FLAGS_tsk_rotation_seconds,
      messenger_->shared_token_verifier()));
  state_ = kInitialized;
  return Status::OK();
}

Status Master::Start() {
  RETURN_NOT_OK(StartAsync());
  RETURN_NOT_OK(WaitForCatalogManagerInit());
  google::FlushLogFiles(google::INFO); // Flush the startup messages.
  return Status::OK();
}

Status Master::StartAsync() {
  CHECK_EQ(kInitialized, state_);

  RETURN_NOT_OK(maintenance_manager_->Init(fs_manager_->uuid()));

  gscoped_ptr<ServiceIf> impl(new MasterServiceImpl(this));
  gscoped_ptr<ServiceIf> consensus_service(new ConsensusServiceImpl(
      this, catalog_manager_.get()));
  gscoped_ptr<ServiceIf> tablet_copy_service(new TabletCopyServiceImpl(
      this, catalog_manager_.get()));

  RETURN_NOT_OK(RegisterService(std::move(impl)));
  RETURN_NOT_OK(RegisterService(std::move(consensus_service)));
  RETURN_NOT_OK(RegisterService(std::move(tablet_copy_service)));
  RETURN_NOT_OK(KuduServer::Start());

  // Now that we've bound, construct our ServerRegistrationPB.
  RETURN_NOT_OK(InitMasterRegistration());

  // Start initializing the catalog manager.
  RETURN_NOT_OK(init_pool_->SubmitClosure(Bind(&Master::InitCatalogManagerTask,
                                               Unretained(this))));

  state_ = kRunning;

  return Status::OK();
}

void Master::InitCatalogManagerTask() {
  Status s = InitCatalogManager();
  if (!s.ok()) {
    LOG(ERROR) << "Unable to init master catalog manager: " << s.ToString();
  }
  init_status_.Set(s);
}

Status Master::InitCatalogManager() {
  if (catalog_manager_->IsInitialized()) {
    return Status::IllegalState("Catalog manager is already initialized");
  }
  RETURN_NOT_OK_PREPEND(catalog_manager_->Init(is_first_run_),
                        "Unable to initialize catalog manager");
  return Status::OK();
}

Status Master::WaitForCatalogManagerInit() const {
  CHECK_EQ(state_, kRunning);

  return init_status_.Get();
}

Status Master::WaitUntilCatalogManagerIsLeaderAndReadyForTests(const MonoDelta& timeout) {
  Status s;
  MonoTime start = MonoTime::Now();
  int backoff_ms = 1;
  const int kMaxBackoffMs = 256;
  do {
    {
      CatalogManager::ScopedLeaderSharedLock l(catalog_manager_.get());
      if (l.first_failed_status().ok()) {
        return Status::OK();
      }
    }
    SleepFor(MonoDelta::FromMilliseconds(backoff_ms));
    backoff_ms = min(backoff_ms << 1, kMaxBackoffMs);
  } while (MonoTime::Now() < (start + timeout));
  return Status::TimedOut("Maximum time exceeded waiting for master leadership",
                          s.ToString());
}

void Master::Shutdown() {
  if (state_ == kRunning) {
    string name = ToString();
    LOG(INFO) << name << " shutting down...";

    // 1. Stop accepting new RPCs.
    UnregisterAllServices();

    // 2. Shut down the master's subsystems.
    maintenance_manager_->Shutdown();
    catalog_manager_->Shutdown();

    // 3. Shut down generic subsystems.
    KuduServer::Shutdown();
    LOG(INFO) << name << " shutdown complete.";
  }
  state_ = kStopped;
}

Status Master::GetMasterRegistration(ServerRegistrationPB* reg) const {
  if (!registration_initialized_.load(std::memory_order_acquire)) {
    return Status::ServiceUnavailable("Master startup not complete");
  }
  reg->CopyFrom(registration_);
  return Status::OK();
}

Status Master::InitMasterRegistration() {
  CHECK(!registration_initialized_.load());

  ServerRegistrationPB reg;
  vector<Sockaddr> rpc_addrs;
  RETURN_NOT_OK_PREPEND(rpc_server()->GetAdvertisedAddresses(&rpc_addrs),
                        "Couldn't get RPC addresses");
  RETURN_NOT_OK(AddHostPortPBs(rpc_addrs, reg.mutable_rpc_addresses()));

  if (web_server()) {
    vector<Sockaddr> http_addrs;
    RETURN_NOT_OK(web_server()->GetAdvertisedAddresses(&http_addrs));
    RETURN_NOT_OK(AddHostPortPBs(http_addrs, reg.mutable_http_addresses()));
    reg.set_https_enabled(web_server()->IsSecure());
  }
  reg.set_software_version(VersionInfo::GetVersionInfo());

  registration_.Swap(&reg);
  registration_initialized_.store(true);

  return Status::OK();
}

namespace {

// TODO this method should be moved to a separate class (along with
// ListMasters), so that it can also be used in TS and client when
// bootstrapping.
Status GetMasterEntryForHost(const shared_ptr<rpc::Messenger>& messenger,
                             const HostPort& hostport,
                             ServerEntryPB* e) {
  Sockaddr sockaddr;
  RETURN_NOT_OK(SockaddrFromHostPort(hostport, &sockaddr));
  MasterServiceProxy proxy(messenger, sockaddr, hostport.host());
  GetMasterRegistrationRequestPB req;
  GetMasterRegistrationResponsePB resp;
  rpc::RpcController controller;
  controller.set_timeout(MonoDelta::FromMilliseconds(FLAGS_master_registration_rpc_timeout_ms));
  RETURN_NOT_OK(proxy.GetMasterRegistration(req, &resp, &controller));
  e->mutable_instance_id()->CopyFrom(resp.instance_id());
  if (resp.has_error()) {
    return StatusFromPB(resp.error().status());
  }
  e->mutable_registration()->CopyFrom(resp.registration());
  e->set_role(resp.role());
  return Status::OK();
}

} // anonymous namespace

Status Master::ListMasters(std::vector<ServerEntryPB>* masters) const {
  if (!opts_.IsDistributed()) {
    ServerEntryPB local_entry;
    local_entry.mutable_instance_id()->CopyFrom(catalog_manager_->NodeInstance());
    RETURN_NOT_OK(GetMasterRegistration(local_entry.mutable_registration()));
    local_entry.set_role(RaftPeerPB::LEADER);
    masters->push_back(local_entry);
    return Status::OK();
  }

  // Since --master_addresses may contain duplicates, including different names
  // for the same server, we deduplicate the masters by UUID here.
  auto uuid_cmp = [](const ServerEntryPB& left, const ServerEntryPB& right) {
    return left.instance_id().permanent_uuid() < right.instance_id().permanent_uuid();
  };
  std::set<ServerEntryPB, decltype(uuid_cmp)> masters_by_uuid(uuid_cmp);
  for (const HostPort& peer_addr : opts_.master_addresses) {
    ServerEntryPB peer_entry;
    Status s = GetMasterEntryForHost(messenger_, peer_addr, &peer_entry);
    if (!s.ok()) {
      s = s.CloneAndPrepend(
          Substitute("Unable to get registration information for peer ($0)",
                     peer_addr.ToString()));
      LOG(WARNING) << s.ToString();
      StatusToPB(s, peer_entry.mutable_error());
    }
    InsertIfNotPresent(&masters_by_uuid, peer_entry);
  }

  std::copy(masters_by_uuid.begin(), masters_by_uuid.end(), std::back_inserter(*masters));
  return Status::OK();
}

} // namespace master
} // namespace kudu
