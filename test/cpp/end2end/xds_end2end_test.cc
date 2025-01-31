/*
 *
 * Copyright 2017 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#include <grpc/grpc.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <grpc/support/time.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>

#include "src/core/ext/filters/client_channel/backup_poller.h"
#include "src/core/ext/filters/client_channel/parse_address.h"
#include "src/core/ext/filters/client_channel/resolver/fake/fake_resolver.h"
#include "src/core/ext/filters/client_channel/server_address.h"
#include "src/core/lib/gpr/env.h"
#include "src/core/lib/gprpp/ref_counted_ptr.h"
#include "src/core/lib/iomgr/sockaddr.h"
#include "src/core/lib/security/credentials/fake/fake_credentials.h"
#include "src/cpp/client/secure_credentials.h"
#include "src/cpp/server/secure_server_credentials.h"

#include "test/core/util/port.h"
#include "test/core/util/test_config.h"
#include "test/cpp/end2end/test_service_impl.h"

#include "src/proto/grpc/lb/v2/xds_for_test.grpc.pb.h"
#include "src/proto/grpc/testing/echo.grpc.pb.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

// TODO(dgq): Other scenarios in need of testing:
// - Send a serverlist with faulty ip:port addresses (port > 2^16, etc).
// - Test reception of invalid serverlist
// - Test against a non-LB server.
// - Random LB server closing the stream unexpectedly.
//
// Findings from end to end testing to be covered here:
// - Handling of LB servers restart, including reconnection after backing-off
//   retries.
// - Destruction of load balanced channel (and therefore of xds instance)
//   while:
//   1) the internal LB call is still active. This should work by virtue
//   of the weak reference the LB call holds. The call should be terminated as
//   part of the xds shutdown process.
//   2) the retry timer is active. Again, the weak reference it holds should
//   prevent a premature call to \a glb_destroy.

namespace grpc {
namespace testing {
namespace {

using std::chrono::system_clock;

using ::envoy::api::v2::ClusterLoadAssignment;
using ::envoy::api::v2::DiscoveryRequest;
using ::envoy::api::v2::DiscoveryResponse;
using ::envoy::api::v2::EndpointDiscoveryService;

constexpr char kEdsTypeUrl[] =
    "type.googleapis.com/envoy.api.v2.ClusterLoadAssignment";
constexpr char kDefaultLocalityRegion[] = "xds_default_locality_region";
constexpr char kDefaultLocalityZone[] = "xds_default_locality_zone";
constexpr char kDefaultLocalitySubzone[] = "xds_default_locality_subzone";

template <typename ServiceType>
class CountedService : public ServiceType {
 public:
  size_t request_count() {
    grpc::internal::MutexLock lock(&mu_);
    return request_count_;
  }

  size_t response_count() {
    grpc::internal::MutexLock lock(&mu_);
    return response_count_;
  }

  void IncreaseResponseCount() {
    grpc::internal::MutexLock lock(&mu_);
    ++response_count_;
  }
  void IncreaseRequestCount() {
    grpc::internal::MutexLock lock(&mu_);
    ++request_count_;
  }

  void ResetCounters() {
    grpc::internal::MutexLock lock(&mu_);
    request_count_ = 0;
    response_count_ = 0;
  }

 protected:
  grpc::internal::Mutex mu_;

 private:
  size_t request_count_ = 0;
  size_t response_count_ = 0;
};

using BackendService = CountedService<TestServiceImpl>;
using BalancerService = CountedService<EndpointDiscoveryService::Service>;

const char g_kCallCredsMdKey[] = "Balancer should not ...";
const char g_kCallCredsMdValue[] = "... receive me";

class BackendServiceImpl : public BackendService {
 public:
  BackendServiceImpl() {}

  Status Echo(ServerContext* context, const EchoRequest* request,
              EchoResponse* response) override {
    // Backend should receive the call credentials metadata.
    auto call_credentials_entry =
        context->client_metadata().find(g_kCallCredsMdKey);
    EXPECT_NE(call_credentials_entry, context->client_metadata().end());
    if (call_credentials_entry != context->client_metadata().end()) {
      EXPECT_EQ(call_credentials_entry->second, g_kCallCredsMdValue);
    }
    IncreaseRequestCount();
    const auto status = TestServiceImpl::Echo(context, request, response);
    IncreaseResponseCount();
    AddClient(context->peer());
    return status;
  }

  void Shutdown() {}

  std::set<grpc::string> clients() {
    grpc::internal::MutexLock lock(&clients_mu_);
    return clients_;
  }

 private:
  void AddClient(const grpc::string& client) {
    grpc::internal::MutexLock lock(&clients_mu_);
    clients_.insert(client);
  }

  grpc::internal::Mutex mu_;
  grpc::internal::Mutex clients_mu_;
  std::set<grpc::string> clients_;
};

struct ClientStats {
  size_t num_calls_started = 0;
  size_t num_calls_finished = 0;
  size_t num_calls_finished_with_client_failed_to_send = 0;
  size_t num_calls_finished_known_received = 0;
  std::map<grpc::string, size_t> drop_token_counts;

  ClientStats& operator+=(const ClientStats& other) {
    num_calls_started += other.num_calls_started;
    num_calls_finished += other.num_calls_finished;
    num_calls_finished_with_client_failed_to_send +=
        other.num_calls_finished_with_client_failed_to_send;
    num_calls_finished_known_received +=
        other.num_calls_finished_known_received;
    for (const auto& p : other.drop_token_counts) {
      drop_token_counts[p.first] += p.second;
    }
    return *this;
  }

  void Reset() {
    num_calls_started = 0;
    num_calls_finished = 0;
    num_calls_finished_with_client_failed_to_send = 0;
    num_calls_finished_known_received = 0;
    drop_token_counts.clear();
  }
};

class BalancerServiceImpl : public BalancerService {
 public:
  using Stream = ServerReaderWriter<DiscoveryResponse, DiscoveryRequest>;
  using ResponseDelayPair = std::pair<DiscoveryResponse, int>;

  explicit BalancerServiceImpl(int client_load_reporting_interval_seconds)
      : client_load_reporting_interval_seconds_(
            client_load_reporting_interval_seconds) {}

  Status StreamEndpoints(ServerContext* context, Stream* stream) override {
    // TODO(juanlishen): Clean up the scoping.
    gpr_log(GPR_INFO, "LB[%p]: EDS starts", this);
    {
      grpc::internal::MutexLock lock(&mu_);
      if (serverlist_done_) goto done;
    }
    {
      // Balancer shouldn't receive the call credentials metadata.
      EXPECT_EQ(context->client_metadata().find(g_kCallCredsMdKey),
                context->client_metadata().end());
      DiscoveryRequest request;
      std::vector<ResponseDelayPair> responses_and_delays;
      if (!stream->Read(&request)) goto done;
      IncreaseRequestCount();
      gpr_log(GPR_INFO, "LB[%p]: received initial message '%s'", this,
              request.DebugString().c_str());
      {
        grpc::internal::MutexLock lock(&mu_);
        responses_and_delays = responses_and_delays_;
      }
      for (const auto& response_and_delay : responses_and_delays) {
        SendResponse(stream, response_and_delay.first,
                     response_and_delay.second);
      }
      {
        grpc::internal::MutexLock lock(&mu_);
        serverlist_cond_.WaitUntil(&mu_, [this] { return serverlist_done_; });
      }
      if (client_load_reporting_interval_seconds_ > 0) {
        // TODO(juanlishen): Use LRS to receive load report.
      }
    }
  done:
    gpr_log(GPR_INFO, "LB[%p]: done", this);
    return Status::OK;
  }

  void add_response(const DiscoveryResponse& response, int send_after_ms) {
    grpc::internal::MutexLock lock(&mu_);
    responses_and_delays_.push_back(std::make_pair(response, send_after_ms));
  }

  void Shutdown() {
    grpc::internal::MutexLock lock(&mu_);
    NotifyDoneWithServerlistsLocked();
    responses_and_delays_.clear();
    gpr_log(GPR_INFO, "LB[%p]: shut down", this);
  }

  static DiscoveryResponse BuildResponseForBackends(
      const std::vector<int>& backend_ports,
      const std::map<grpc::string, size_t>& drop_token_counts) {
    ClusterLoadAssignment assignment;
    assignment.set_cluster_name("service name");
    auto* endpoints = assignment.add_endpoints();
    endpoints->mutable_load_balancing_weight()->set_value(3);
    endpoints->set_priority(0);
    endpoints->mutable_locality()->set_region(kDefaultLocalityRegion);
    endpoints->mutable_locality()->set_zone(kDefaultLocalityZone);
    endpoints->mutable_locality()->set_sub_zone(kDefaultLocalitySubzone);
    for (const int& backend_port : backend_ports) {
      auto* lb_endpoints = endpoints->add_lb_endpoints();
      auto* endpoint = lb_endpoints->mutable_endpoint();
      auto* address = endpoint->mutable_address();
      auto* socket_address = address->mutable_socket_address();
      socket_address->set_address("127.0.0.1");
      socket_address->set_port_value(backend_port);
    }
    DiscoveryResponse response;
    response.set_type_url(kEdsTypeUrl);
    response.add_resources()->PackFrom(assignment);
    return response;
  }

  void NotifyDoneWithServerlists() {
    grpc::internal::MutexLock lock(&mu_);
    NotifyDoneWithServerlistsLocked();
  }

  void NotifyDoneWithServerlistsLocked() {
    if (!serverlist_done_) {
      serverlist_done_ = true;
      serverlist_cond_.Broadcast();
    }
  }

 private:
  void SendResponse(Stream* stream, const DiscoveryResponse& response,
                    int delay_ms) {
    gpr_log(GPR_INFO, "LB[%p]: sleeping for %d ms...", this, delay_ms);
    if (delay_ms > 0) {
      gpr_sleep_until(grpc_timeout_milliseconds_to_deadline(delay_ms));
    }
    gpr_log(GPR_INFO, "LB[%p]: Woke up! Sending response '%s'", this,
            response.DebugString().c_str());
    IncreaseResponseCount();
    stream->Write(response);
  }

  const int client_load_reporting_interval_seconds_;
  std::vector<ResponseDelayPair> responses_and_delays_;
  grpc::internal::Mutex mu_;
  grpc::internal::CondVar serverlist_cond_;
  bool serverlist_done_ = false;
};

class XdsEnd2endTest : public ::testing::Test {
 protected:
  XdsEnd2endTest(size_t num_backends, size_t num_balancers,
                 int client_load_reporting_interval_seconds)
      : server_host_("localhost"),
        num_backends_(num_backends),
        num_balancers_(num_balancers),
        client_load_reporting_interval_seconds_(
            client_load_reporting_interval_seconds) {}

  static void SetUpTestCase() {
    // Make the backup poller poll very frequently in order to pick up
    // updates from all the subchannels's FDs.
    GPR_GLOBAL_CONFIG_SET(grpc_client_channel_backup_poll_interval_ms, 1);
    grpc_init();
  }

  static void TearDownTestCase() { grpc_shutdown(); }

  void SetUp() override {
    response_generator_ =
        grpc_core::MakeRefCounted<grpc_core::FakeResolverResponseGenerator>();
    lb_channel_response_generator_ =
        grpc_core::MakeRefCounted<grpc_core::FakeResolverResponseGenerator>();
    // Start the backends.
    for (size_t i = 0; i < num_backends_; ++i) {
      backends_.emplace_back(new ServerThread<BackendServiceImpl>("backend"));
      backends_.back()->Start(server_host_);
    }
    // Start the load balancers.
    for (size_t i = 0; i < num_balancers_; ++i) {
      balancers_.emplace_back(new ServerThread<BalancerServiceImpl>(
          "balancer", client_load_reporting_interval_seconds_));
      balancers_.back()->Start(server_host_);
    }
    ResetStub();
  }

  void TearDown() override {
    ShutdownAllBackends();
    for (auto& balancer : balancers_) balancer->Shutdown();
  }

  void StartAllBackends() {
    for (auto& backend : backends_) backend->Start(server_host_);
  }

  void StartBackend(size_t index) { backends_[index]->Start(server_host_); }

  void ShutdownAllBackends() {
    for (auto& backend : backends_) backend->Shutdown();
  }

  void ShutdownBackend(size_t index) { backends_[index]->Shutdown(); }

  void ResetStub(int fallback_timeout = 0,
                 const grpc::string& expected_targets = "") {
    ChannelArguments args;
    // TODO(juanlishen): Add setter to ChannelArguments.
    if (fallback_timeout > 0) {
      args.SetInt(GRPC_ARG_XDS_FALLBACK_TIMEOUT_MS, fallback_timeout);
    }
    args.SetPointer(GRPC_ARG_FAKE_RESOLVER_RESPONSE_GENERATOR,
                    response_generator_.get());
    if (!expected_targets.empty()) {
      args.SetString(GRPC_ARG_FAKE_SECURITY_EXPECTED_TARGETS, expected_targets);
    }
    std::ostringstream uri;
    uri << "fake:///" << kApplicationTargetName_;
    // TODO(dgq): templatize tests to run everything using both secure and
    // insecure channel credentials.
    grpc_channel_credentials* channel_creds =
        grpc_fake_transport_security_credentials_create();
    grpc_call_credentials* call_creds = grpc_md_only_test_credentials_create(
        g_kCallCredsMdKey, g_kCallCredsMdValue, false);
    std::shared_ptr<ChannelCredentials> creds(
        new SecureChannelCredentials(grpc_composite_channel_credentials_create(
            channel_creds, call_creds, nullptr)));
    call_creds->Unref();
    channel_creds->Unref();
    channel_ = ::grpc::CreateCustomChannel(uri.str(), creds, args);
    stub_ = grpc::testing::EchoTestService::NewStub(channel_);
  }

  void ResetBackendCounters() {
    for (auto& backend : backends_) backend->service_.ResetCounters();
  }

  ClientStats WaitForLoadReports() {
    ClientStats client_stats;
    // TODO(juanlishen): Wait in LRS.
    return client_stats;
  }

  bool SeenAllBackends(size_t start_index = 0, size_t stop_index = 0) {
    if (stop_index == 0) stop_index = backends_.size();
    for (size_t i = start_index; i < stop_index; ++i) {
      if (backends_[i]->service_.request_count() == 0) return false;
    }
    return true;
  }

  void SendRpcAndCount(int* num_total, int* num_ok, int* num_failure,
                       int* num_drops) {
    const Status status = SendRpc();
    if (status.ok()) {
      ++*num_ok;
    } else {
      if (status.error_message() == "Call dropped by load balancing policy") {
        ++*num_drops;
      } else {
        ++*num_failure;
      }
    }
    ++*num_total;
  }

  std::tuple<int, int, int> WaitForAllBackends(int num_requests_multiple_of = 1,
                                               size_t start_index = 0,
                                               size_t stop_index = 0) {
    int num_ok = 0;
    int num_failure = 0;
    int num_drops = 0;
    int num_total = 0;
    while (!SeenAllBackends(start_index, stop_index)) {
      SendRpcAndCount(&num_total, &num_ok, &num_failure, &num_drops);
    }
    while (num_total % num_requests_multiple_of != 0) {
      SendRpcAndCount(&num_total, &num_ok, &num_failure, &num_drops);
    }
    ResetBackendCounters();
    gpr_log(GPR_INFO,
            "Performed %d warm up requests (a multiple of %d) against the "
            "backends. %d succeeded, %d failed, %d dropped.",
            num_total, num_requests_multiple_of, num_ok, num_failure,
            num_drops);
    return std::make_tuple(num_ok, num_failure, num_drops);
  }

  void WaitForBackend(size_t backend_idx) {
    do {
      (void)SendRpc();
    } while (backends_[backend_idx]->service_.request_count() == 0);
    ResetBackendCounters();
  }

  grpc_core::ServerAddressList CreateLbAddressesFromPortList(
      const std::vector<int>& ports) {
    grpc_core::ServerAddressList addresses;
    for (int port : ports) {
      char* lb_uri_str;
      gpr_asprintf(&lb_uri_str, "ipv4:127.0.0.1:%d", port);
      grpc_uri* lb_uri = grpc_uri_parse(lb_uri_str, true);
      GPR_ASSERT(lb_uri != nullptr);
      grpc_resolved_address address;
      GPR_ASSERT(grpc_parse_uri(lb_uri, &address));
      std::vector<grpc_arg> args_to_add;
      grpc_channel_args* args = grpc_channel_args_copy_and_add(
          nullptr, args_to_add.data(), args_to_add.size());
      addresses.emplace_back(address.addr, address.len, args);
      grpc_uri_destroy(lb_uri);
      gpr_free(lb_uri_str);
    }
    return addresses;
  }

  void SetNextResolution(const std::vector<int>& ports,
                         const char* service_config_json = nullptr,
                         grpc_core::FakeResolverResponseGenerator*
                             lb_channel_response_generator = nullptr) {
    grpc_core::ExecCtx exec_ctx;
    grpc_core::Resolver::Result result;
    result.addresses = CreateLbAddressesFromPortList(ports);
    if (service_config_json != nullptr) {
      grpc_error* error = GRPC_ERROR_NONE;
      result.service_config =
          grpc_core::ServiceConfig::Create(service_config_json, &error);
      GRPC_ERROR_UNREF(error);
    }
    grpc_arg arg = grpc_core::FakeResolverResponseGenerator::MakeChannelArg(
        lb_channel_response_generator == nullptr
            ? lb_channel_response_generator_.get()
            : lb_channel_response_generator);
    result.args = grpc_channel_args_copy_and_add(nullptr, &arg, 1);
    response_generator_->SetResponse(std::move(result));
  }

  void SetNextResolutionForLbChannelAllBalancers(
      const char* service_config_json = nullptr,
      grpc_core::FakeResolverResponseGenerator* lb_channel_response_generator =
          nullptr) {
    std::vector<int> ports;
    for (size_t i = 0; i < balancers_.size(); ++i) {
      ports.emplace_back(balancers_[i]->port_);
    }
    SetNextResolutionForLbChannel(ports, service_config_json,
                                  lb_channel_response_generator);
  }

  void SetNextResolutionForLbChannel(
      const std::vector<int>& ports, const char* service_config_json = nullptr,
      grpc_core::FakeResolverResponseGenerator* lb_channel_response_generator =
          nullptr) {
    grpc_core::ExecCtx exec_ctx;
    grpc_core::Resolver::Result result;
    result.addresses = CreateLbAddressesFromPortList(ports);
    if (service_config_json != nullptr) {
      grpc_error* error = GRPC_ERROR_NONE;
      result.service_config =
          grpc_core::ServiceConfig::Create(service_config_json, &error);
      GRPC_ERROR_UNREF(error);
    }
    if (lb_channel_response_generator == nullptr) {
      lb_channel_response_generator = lb_channel_response_generator_.get();
    }
    lb_channel_response_generator->SetResponse(std::move(result));
  }

  void SetNextReresolutionResponse(const std::vector<int>& ports) {
    grpc_core::ExecCtx exec_ctx;
    grpc_core::Resolver::Result result;
    result.addresses = CreateLbAddressesFromPortList(ports);
    response_generator_->SetReresolutionResponse(std::move(result));
  }

  const std::vector<int> GetBackendPorts(size_t start_index = 0,
                                         size_t stop_index = 0) const {
    if (stop_index == 0) stop_index = backends_.size();
    std::vector<int> backend_ports;
    for (size_t i = start_index; i < stop_index; ++i) {
      backend_ports.push_back(backends_[i]->port_);
    }
    return backend_ports;
  }

  void ScheduleResponseForBalancer(size_t i, const DiscoveryResponse& response,
                                   int delay_ms) {
    balancers_[i]->service_.add_response(response, delay_ms);
  }

  Status SendRpc(EchoResponse* response = nullptr, int timeout_ms = 1000,
                 bool wait_for_ready = false) {
    const bool local_response = (response == nullptr);
    if (local_response) response = new EchoResponse;
    EchoRequest request;
    request.set_message(kRequestMessage_);
    ClientContext context;
    context.set_deadline(grpc_timeout_milliseconds_to_deadline(timeout_ms));
    if (wait_for_ready) context.set_wait_for_ready(true);
    Status status = stub_->Echo(&context, request, response);
    if (local_response) delete response;
    return status;
  }

  void CheckRpcSendOk(const size_t times = 1, const int timeout_ms = 1000,
                      bool wait_for_ready = false) {
    for (size_t i = 0; i < times; ++i) {
      EchoResponse response;
      const Status status = SendRpc(&response, timeout_ms, wait_for_ready);
      EXPECT_TRUE(status.ok()) << "code=" << status.error_code()
                               << " message=" << status.error_message();
      EXPECT_EQ(response.message(), kRequestMessage_);
    }
  }

  void CheckRpcSendFailure() {
    const Status status = SendRpc();
    EXPECT_FALSE(status.ok());
  }

  template <typename T>
  struct ServerThread {
    template <typename... Args>
    explicit ServerThread(const grpc::string& type, Args&&... args)
        : port_(grpc_pick_unused_port_or_die()),
          type_(type),
          service_(std::forward<Args>(args)...) {}

    void Start(const grpc::string& server_host) {
      gpr_log(GPR_INFO, "starting %s server on port %d", type_.c_str(), port_);
      GPR_ASSERT(!running_);
      running_ = true;
      grpc::internal::Mutex mu;
      // We need to acquire the lock here in order to prevent the notify_one
      // by ServerThread::Serve from firing before the wait below is hit.
      grpc::internal::MutexLock lock(&mu);
      grpc::internal::CondVar cond;
      thread_.reset(new std::thread(
          std::bind(&ServerThread::Serve, this, server_host, &mu, &cond)));
      cond.Wait(&mu);
      gpr_log(GPR_INFO, "%s server startup complete", type_.c_str());
    }

    void Serve(const grpc::string& server_host, grpc::internal::Mutex* mu,
               grpc::internal::CondVar* cond) {
      // We need to acquire the lock here in order to prevent the notify_one
      // below from firing before its corresponding wait is executed.
      grpc::internal::MutexLock lock(mu);
      std::ostringstream server_address;
      server_address << server_host << ":" << port_;
      ServerBuilder builder;
      std::shared_ptr<ServerCredentials> creds(new SecureServerCredentials(
          grpc_fake_transport_security_server_credentials_create()));
      builder.AddListeningPort(server_address.str(), creds);
      builder.RegisterService(&service_);
      server_ = builder.BuildAndStart();
      cond->Signal();
    }

    void Shutdown() {
      if (!running_) return;
      gpr_log(GPR_INFO, "%s about to shutdown", type_.c_str());
      service_.Shutdown();
      server_->Shutdown(grpc_timeout_milliseconds_to_deadline(0));
      thread_->join();
      gpr_log(GPR_INFO, "%s shutdown completed", type_.c_str());
      running_ = false;
    }

    const int port_;
    grpc::string type_;
    T service_;
    std::unique_ptr<Server> server_;
    std::unique_ptr<std::thread> thread_;
    bool running_ = false;
  };

  const grpc::string server_host_;
  const size_t num_backends_;
  const size_t num_balancers_;
  const int client_load_reporting_interval_seconds_;
  std::shared_ptr<Channel> channel_;
  std::unique_ptr<grpc::testing::EchoTestService::Stub> stub_;
  std::vector<std::unique_ptr<ServerThread<BackendServiceImpl>>> backends_;
  std::vector<std::unique_ptr<ServerThread<BalancerServiceImpl>>> balancers_;
  grpc_core::RefCountedPtr<grpc_core::FakeResolverResponseGenerator>
      response_generator_;
  grpc_core::RefCountedPtr<grpc_core::FakeResolverResponseGenerator>
      lb_channel_response_generator_;
  const grpc::string kRequestMessage_ = "Live long and prosper.";
  const grpc::string kApplicationTargetName_ = "application_target_name";
  const grpc::string kDefaultServiceConfig_ =
      "{\n"
      "  \"loadBalancingConfig\":[\n"
      "    { \"does_not_exist\":{} },\n"
      "    { \"xds_experimental\":{ \"balancerName\": \"fake:///lb\" } }\n"
      "  ]\n"
      "}";
};

class SingleBalancerTest : public XdsEnd2endTest {
 public:
  SingleBalancerTest() : XdsEnd2endTest(4, 1, 0) {}
};

TEST_F(SingleBalancerTest, Vanilla) {
  SetNextResolution({}, kDefaultServiceConfig_.c_str());
  SetNextResolutionForLbChannelAllBalancers();
  const size_t kNumRpcsPerAddress = 100;
  ScheduleResponseForBalancer(
      0, BalancerServiceImpl::BuildResponseForBackends(GetBackendPorts(), {}),
      0);
  // Make sure that trying to connect works without a call.
  channel_->GetState(true /* try_to_connect */);
  // We need to wait for all backends to come online.
  WaitForAllBackends();
  // Send kNumRpcsPerAddress RPCs per server.
  CheckRpcSendOk(kNumRpcsPerAddress * num_backends_);
  // Each backend should have gotten 100 requests.
  for (size_t i = 0; i < backends_.size(); ++i) {
    EXPECT_EQ(kNumRpcsPerAddress, backends_[i]->service_.request_count());
  }
  balancers_[0]->service_.NotifyDoneWithServerlists();
  // The balancer got a single request.
  EXPECT_EQ(1U, balancers_[0]->service_.request_count());
  // and sent a single response.
  EXPECT_EQ(1U, balancers_[0]->service_.response_count());

  // Check LB policy name for the channel.
  EXPECT_EQ("xds_experimental", channel_->GetLoadBalancingPolicyName());
}

TEST_F(SingleBalancerTest, SameBackendListedMultipleTimes) {
  SetNextResolution({}, kDefaultServiceConfig_.c_str());
  SetNextResolutionForLbChannelAllBalancers();
  // Same backend listed twice.
  std::vector<int> ports;
  ports.push_back(backends_[0]->port_);
  ports.push_back(backends_[0]->port_);
  const size_t kNumRpcsPerAddress = 10;
  ScheduleResponseForBalancer(
      0, BalancerServiceImpl::BuildResponseForBackends(ports, {}), 0);
  // We need to wait for the backend to come online.
  WaitForBackend(0);
  // Send kNumRpcsPerAddress RPCs per server.
  CheckRpcSendOk(kNumRpcsPerAddress * ports.size());
  // Backend should have gotten 20 requests.
  EXPECT_EQ(kNumRpcsPerAddress * 2, backends_[0]->service_.request_count());
  // And they should have come from a single client port, because of
  // subchannel sharing.
  EXPECT_EQ(1UL, backends_[0]->service_.clients().size());
  balancers_[0]->service_.NotifyDoneWithServerlists();
}

TEST_F(SingleBalancerTest, SecureNaming) {
  // TODO(juanlishen): Use separate fake creds for the balancer channel.
  ResetStub(0, kApplicationTargetName_ + ";lb");
  SetNextResolution({}, kDefaultServiceConfig_.c_str());
  SetNextResolutionForLbChannel({balancers_[0]->port_});
  const size_t kNumRpcsPerAddress = 100;
  ScheduleResponseForBalancer(
      0, BalancerServiceImpl::BuildResponseForBackends(GetBackendPorts(), {}),
      0);
  // Make sure that trying to connect works without a call.
  channel_->GetState(true /* try_to_connect */);
  // We need to wait for all backends to come online.
  WaitForAllBackends();
  // Send kNumRpcsPerAddress RPCs per server.
  CheckRpcSendOk(kNumRpcsPerAddress * num_backends_);

  // Each backend should have gotten 100 requests.
  for (size_t i = 0; i < backends_.size(); ++i) {
    EXPECT_EQ(kNumRpcsPerAddress, backends_[i]->service_.request_count());
  }
  // The balancer got a single request.
  EXPECT_EQ(1U, balancers_[0]->service_.request_count());
  // and sent a single response.
  EXPECT_EQ(1U, balancers_[0]->service_.response_count());
}

TEST_F(SingleBalancerTest, SecureNamingDeathTest) {
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  // Make sure that we blow up (via abort() from the security connector) when
  // the name from the balancer doesn't match expectations.
  ASSERT_DEATH_IF_SUPPORTED(
      {
        ResetStub(0, kApplicationTargetName_ + ";lb");
        SetNextResolution({},
                          "{\n"
                          "  \"loadBalancingConfig\":[\n"
                          "    { \"does_not_exist\":{} },\n"
                          "    { \"xds_experimental\":{ \"balancerName\": "
                          "\"fake:///wrong_lb\" } }\n"
                          "  ]\n"
                          "}");
        SetNextResolutionForLbChannel({balancers_[0]->port_});
        channel_->WaitForConnected(grpc_timeout_seconds_to_deadline(1));
      },
      "");
}

TEST_F(SingleBalancerTest, InitiallyEmptyServerlist) {
  SetNextResolution({}, kDefaultServiceConfig_.c_str());
  SetNextResolutionForLbChannelAllBalancers();
  const int kServerlistDelayMs = 500 * grpc_test_slowdown_factor();
  const int kCallDeadlineMs = kServerlistDelayMs * 2;
  // First response is an empty serverlist, sent right away.
  ScheduleResponseForBalancer(
      0, BalancerServiceImpl::BuildResponseForBackends({}, {}), 0);
  // Send non-empty serverlist only after kServerlistDelayMs
  ScheduleResponseForBalancer(
      0, BalancerServiceImpl::BuildResponseForBackends(GetBackendPorts(), {}),
      kServerlistDelayMs);
  const auto t0 = system_clock::now();
  // Client will block: LB will initially send empty serverlist.
  CheckRpcSendOk(1, kCallDeadlineMs, true /* wait_for_ready */);
  const auto ellapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          system_clock::now() - t0);
  // but eventually, the LB sends a serverlist update that allows the call to
  // proceed. The call delay must be larger than the delay in sending the
  // populated serverlist but under the call's deadline (which is enforced by
  // the call's deadline).
  EXPECT_GT(ellapsed_ms.count(), kServerlistDelayMs);
  balancers_[0]->service_.NotifyDoneWithServerlists();
  // The balancer got a single request.
  EXPECT_EQ(1U, balancers_[0]->service_.request_count());
  // and sent two responses.
  EXPECT_EQ(2U, balancers_[0]->service_.response_count());
}

TEST_F(SingleBalancerTest, AllServersUnreachableFailFast) {
  SetNextResolution({}, kDefaultServiceConfig_.c_str());
  SetNextResolutionForLbChannelAllBalancers();
  const size_t kNumUnreachableServers = 5;
  std::vector<int> ports;
  for (size_t i = 0; i < kNumUnreachableServers; ++i) {
    ports.push_back(grpc_pick_unused_port_or_die());
  }
  ScheduleResponseForBalancer(
      0, BalancerServiceImpl::BuildResponseForBackends(ports, {}), 0);
  const Status status = SendRpc();
  // The error shouldn't be DEADLINE_EXCEEDED.
  EXPECT_EQ(StatusCode::UNAVAILABLE, status.error_code());
  balancers_[0]->service_.NotifyDoneWithServerlists();
  // The balancer got a single request.
  EXPECT_EQ(1U, balancers_[0]->service_.request_count());
  // and sent a single response.
  EXPECT_EQ(1U, balancers_[0]->service_.response_count());
}

TEST_F(SingleBalancerTest, Fallback) {
  const int kFallbackTimeoutMs = 200 * grpc_test_slowdown_factor();
  const int kServerlistDelayMs = 500 * grpc_test_slowdown_factor();
  const size_t kNumBackendsInResolution = backends_.size() / 2;
  ResetStub(kFallbackTimeoutMs);
  SetNextResolution(GetBackendPorts(0, kNumBackendsInResolution),
                    kDefaultServiceConfig_.c_str());
  SetNextResolutionForLbChannelAllBalancers();
  // Send non-empty serverlist only after kServerlistDelayMs.
  ScheduleResponseForBalancer(
      0,
      BalancerServiceImpl::BuildResponseForBackends(
          GetBackendPorts(kNumBackendsInResolution /* start_index */), {}),
      kServerlistDelayMs);
  // Wait until all the fallback backends are reachable.
  WaitForAllBackends(1 /* num_requests_multiple_of */, 0 /* start_index */,
                     kNumBackendsInResolution /* stop_index */);
  gpr_log(GPR_INFO, "========= BEFORE FIRST BATCH ==========");
  CheckRpcSendOk(kNumBackendsInResolution);
  gpr_log(GPR_INFO, "========= DONE WITH FIRST BATCH ==========");
  // Fallback is used: each backend returned by the resolver should have
  // gotten one request.
  for (size_t i = 0; i < kNumBackendsInResolution; ++i) {
    EXPECT_EQ(1U, backends_[i]->service_.request_count());
  }
  for (size_t i = kNumBackendsInResolution; i < backends_.size(); ++i) {
    EXPECT_EQ(0U, backends_[i]->service_.request_count());
  }
  // Wait until the serverlist reception has been processed and all backends
  // in the serverlist are reachable.
  WaitForAllBackends(1 /* num_requests_multiple_of */,
                     kNumBackendsInResolution /* start_index */);
  gpr_log(GPR_INFO, "========= BEFORE SECOND BATCH ==========");
  CheckRpcSendOk(backends_.size() - kNumBackendsInResolution);
  gpr_log(GPR_INFO, "========= DONE WITH SECOND BATCH ==========");
  // Serverlist is used: each backend returned by the balancer should
  // have gotten one request.
  for (size_t i = 0; i < kNumBackendsInResolution; ++i) {
    EXPECT_EQ(0U, backends_[i]->service_.request_count());
  }
  for (size_t i = kNumBackendsInResolution; i < backends_.size(); ++i) {
    EXPECT_EQ(1U, backends_[i]->service_.request_count());
  }
  // The balancer got a single request.
  EXPECT_EQ(1U, balancers_[0]->service_.request_count());
  // and sent a single response.
  EXPECT_EQ(1U, balancers_[0]->service_.response_count());
}

TEST_F(SingleBalancerTest, FallbackUpdate) {
  const int kFallbackTimeoutMs = 200 * grpc_test_slowdown_factor();
  const int kServerlistDelayMs = 500 * grpc_test_slowdown_factor();
  const size_t kNumBackendsInResolution = backends_.size() / 3;
  const size_t kNumBackendsInResolutionUpdate = backends_.size() / 3;
  ResetStub(kFallbackTimeoutMs);
  SetNextResolution(GetBackendPorts(0, kNumBackendsInResolution),
                    kDefaultServiceConfig_.c_str());
  SetNextResolutionForLbChannelAllBalancers();
  // Send non-empty serverlist only after kServerlistDelayMs.
  ScheduleResponseForBalancer(
      0,
      BalancerServiceImpl::BuildResponseForBackends(
          GetBackendPorts(kNumBackendsInResolution +
                          kNumBackendsInResolutionUpdate /* start_index */),
          {}),
      kServerlistDelayMs);
  // Wait until all the fallback backends are reachable.
  WaitForAllBackends(1 /* num_requests_multiple_of */, 0 /* start_index */,
                     kNumBackendsInResolution /* stop_index */);
  gpr_log(GPR_INFO, "========= BEFORE FIRST BATCH ==========");
  CheckRpcSendOk(kNumBackendsInResolution);
  gpr_log(GPR_INFO, "========= DONE WITH FIRST BATCH ==========");
  // Fallback is used: each backend returned by the resolver should have
  // gotten one request.
  for (size_t i = 0; i < kNumBackendsInResolution; ++i) {
    EXPECT_EQ(1U, backends_[i]->service_.request_count());
  }
  for (size_t i = kNumBackendsInResolution; i < backends_.size(); ++i) {
    EXPECT_EQ(0U, backends_[i]->service_.request_count());
  }
  SetNextResolution(GetBackendPorts(kNumBackendsInResolution,
                                    kNumBackendsInResolution +
                                        kNumBackendsInResolutionUpdate),
                    kDefaultServiceConfig_.c_str());
  // Wait until the resolution update has been processed and all the new
  // fallback backends are reachable.
  WaitForAllBackends(1 /* num_requests_multiple_of */,
                     kNumBackendsInResolution /* start_index */,
                     kNumBackendsInResolution +
                         kNumBackendsInResolutionUpdate /* stop_index */);
  gpr_log(GPR_INFO, "========= BEFORE SECOND BATCH ==========");
  CheckRpcSendOk(kNumBackendsInResolutionUpdate);
  gpr_log(GPR_INFO, "========= DONE WITH SECOND BATCH ==========");
  // The resolution update is used: each backend in the resolution update should
  // have gotten one request.
  for (size_t i = 0; i < kNumBackendsInResolution; ++i) {
    EXPECT_EQ(0U, backends_[i]->service_.request_count());
  }
  for (size_t i = kNumBackendsInResolution;
       i < kNumBackendsInResolution + kNumBackendsInResolutionUpdate; ++i) {
    EXPECT_EQ(1U, backends_[i]->service_.request_count());
  }
  for (size_t i = kNumBackendsInResolution + kNumBackendsInResolutionUpdate;
       i < backends_.size(); ++i) {
    EXPECT_EQ(0U, backends_[i]->service_.request_count());
  }
  // Wait until the serverlist reception has been processed and all backends
  // in the serverlist are reachable.
  WaitForAllBackends(1 /* num_requests_multiple_of */,
                     kNumBackendsInResolution +
                         kNumBackendsInResolutionUpdate /* start_index */);
  gpr_log(GPR_INFO, "========= BEFORE THIRD BATCH ==========");
  CheckRpcSendOk(backends_.size() - kNumBackendsInResolution -
                 kNumBackendsInResolutionUpdate);
  gpr_log(GPR_INFO, "========= DONE WITH THIRD BATCH ==========");
  // Serverlist is used: each backend returned by the balancer should
  // have gotten one request.
  for (size_t i = 0;
       i < kNumBackendsInResolution + kNumBackendsInResolutionUpdate; ++i) {
    EXPECT_EQ(0U, backends_[i]->service_.request_count());
  }
  for (size_t i = kNumBackendsInResolution + kNumBackendsInResolutionUpdate;
       i < backends_.size(); ++i) {
    EXPECT_EQ(1U, backends_[i]->service_.request_count());
  }
  // The balancer got a single request.
  EXPECT_EQ(1U, balancers_[0]->service_.request_count());
  // and sent a single response.
  EXPECT_EQ(1U, balancers_[0]->service_.response_count());
}

TEST_F(SingleBalancerTest, FallbackEarlyWhenBalancerChannelFails) {
  const int kFallbackTimeoutMs = 10000 * grpc_test_slowdown_factor();
  ResetStub(kFallbackTimeoutMs);
  // Return an unreachable balancer and one fallback backend.
  SetNextResolution({backends_[0]->port_}, kDefaultServiceConfig_.c_str());
  SetNextResolutionForLbChannel({grpc_pick_unused_port_or_die()});
  // Send RPC with deadline less than the fallback timeout and make sure it
  // succeeds.
  CheckRpcSendOk(/* times */ 1, /* timeout_ms */ 1000,
                 /* wait_for_ready */ false);
}

TEST_F(SingleBalancerTest, FallbackEarlyWhenBalancerCallFails) {
  const int kFallbackTimeoutMs = 10000 * grpc_test_slowdown_factor();
  ResetStub(kFallbackTimeoutMs);
  // Return one balancer and one fallback backend.
  SetNextResolution({backends_[0]->port_}, kDefaultServiceConfig_.c_str());
  SetNextResolutionForLbChannelAllBalancers();
  // Balancer drops call without sending a serverlist.
  balancers_[0]->service_.NotifyDoneWithServerlists();
  // Send RPC with deadline less than the fallback timeout and make sure it
  // succeeds.
  CheckRpcSendOk(/* times */ 1, /* timeout_ms */ 1000,
                 /* wait_for_ready */ false);
}

TEST_F(SingleBalancerTest, FallbackIfResponseReceivedButChildNotReady) {
  const int kFallbackTimeoutMs = 500 * grpc_test_slowdown_factor();
  ResetStub(kFallbackTimeoutMs);
  SetNextResolution({backends_[0]->port_}, kDefaultServiceConfig_.c_str());
  SetNextResolutionForLbChannelAllBalancers();
  // Send a serverlist that only contains an unreachable backend before fallback
  // timeout.
  ScheduleResponseForBalancer(0,
                              BalancerServiceImpl::BuildResponseForBackends(
                                  {grpc_pick_unused_port_or_die()}, {}),
                              0);
  // Because no child policy is ready before fallback timeout, we enter fallback
  // mode.
  WaitForBackend(0);
}

TEST_F(SingleBalancerTest, FallbackModeIsExitedWhenBalancerSaysToDropAllCalls) {
  // Return an unreachable balancer and one fallback backend.
  SetNextResolution({backends_[0]->port_}, kDefaultServiceConfig_.c_str());
  SetNextResolutionForLbChannel({grpc_pick_unused_port_or_die()});
  // Enter fallback mode because the LB channel fails to connect.
  WaitForBackend(0);
  // Return a new balancer that sends an empty serverlist.
  ScheduleResponseForBalancer(
      0, BalancerServiceImpl::BuildResponseForBackends({}, {}), 0);
  SetNextResolutionForLbChannelAllBalancers();
  // Send RPCs until failure.
  gpr_timespec deadline = gpr_time_add(
      gpr_now(GPR_CLOCK_REALTIME), gpr_time_from_millis(5000, GPR_TIMESPAN));
  do {
    auto status = SendRpc();
    if (!status.ok()) break;
  } while (gpr_time_cmp(gpr_now(GPR_CLOCK_REALTIME), deadline) < 0);
  CheckRpcSendFailure();
}

TEST_F(SingleBalancerTest, FallbackModeIsExitedAfterChildRready) {
  // Return an unreachable balancer and one fallback backend.
  SetNextResolution({backends_[0]->port_}, kDefaultServiceConfig_.c_str());
  SetNextResolutionForLbChannel({grpc_pick_unused_port_or_die()});
  // Enter fallback mode because the LB channel fails to connect.
  WaitForBackend(0);
  // Return a new balancer that sends a dead backend.
  ShutdownBackend(1);
  ScheduleResponseForBalancer(
      0,
      BalancerServiceImpl::BuildResponseForBackends({backends_[1]->port_}, {}),
      0);
  SetNextResolutionForLbChannelAllBalancers();
  // The state (TRANSIENT_FAILURE) update from the child policy will be ignored
  // because we are still in fallback mode.
  gpr_timespec deadline = gpr_time_add(
      gpr_now(GPR_CLOCK_REALTIME), gpr_time_from_millis(5000, GPR_TIMESPAN));
  // Send 5 seconds worth of RPCs.
  do {
    CheckRpcSendOk();
  } while (gpr_time_cmp(gpr_now(GPR_CLOCK_REALTIME), deadline) < 0);
  // After the backend is restarted, the child policy will eventually be READY,
  // and we will exit fallback mode.
  StartBackend(1);
  WaitForBackend(1);
  // We have exited fallback mode, so calls will go to the child policy
  // exclusively.
  CheckRpcSendOk(100);
  EXPECT_EQ(0U, backends_[0]->service_.request_count());
  EXPECT_EQ(100U, backends_[1]->service_.request_count());
}

TEST_F(SingleBalancerTest, BackendsRestart) {
  SetNextResolution({}, kDefaultServiceConfig_.c_str());
  SetNextResolutionForLbChannelAllBalancers();
  ScheduleResponseForBalancer(
      0, BalancerServiceImpl::BuildResponseForBackends(GetBackendPorts(), {}),
      0);
  WaitForAllBackends();
  // Stop backends.  RPCs should fail.
  ShutdownAllBackends();
  CheckRpcSendFailure();
  // Restart all backends.  RPCs should start succeeding again.
  StartAllBackends();
  CheckRpcSendOk(1 /* times */, 2000 /* timeout_ms */,
                 true /* wait_for_ready */);
}

class UpdatesTest : public XdsEnd2endTest {
 public:
  UpdatesTest() : XdsEnd2endTest(4, 3, 0) {}
};

TEST_F(UpdatesTest, UpdateBalancersButKeepUsingOriginalBalancer) {
  SetNextResolution({}, kDefaultServiceConfig_.c_str());
  SetNextResolutionForLbChannelAllBalancers();
  const std::vector<int> first_backend{GetBackendPorts()[0]};
  const std::vector<int> second_backend{GetBackendPorts()[1]};
  ScheduleResponseForBalancer(
      0, BalancerServiceImpl::BuildResponseForBackends(first_backend, {}), 0);
  ScheduleResponseForBalancer(
      1, BalancerServiceImpl::BuildResponseForBackends(second_backend, {}), 0);

  // Wait until the first backend is ready.
  WaitForBackend(0);

  // Send 10 requests.
  gpr_log(GPR_INFO, "========= BEFORE FIRST BATCH ==========");
  CheckRpcSendOk(10);
  gpr_log(GPR_INFO, "========= DONE WITH FIRST BATCH ==========");

  // All 10 requests should have gone to the first backend.
  EXPECT_EQ(10U, backends_[0]->service_.request_count());

  // Balancer 0 got a single request.
  EXPECT_EQ(1U, balancers_[0]->service_.request_count());
  // and sent a single response.
  EXPECT_EQ(1U, balancers_[0]->service_.response_count());
  EXPECT_EQ(0U, balancers_[1]->service_.request_count());
  EXPECT_EQ(0U, balancers_[1]->service_.response_count());
  EXPECT_EQ(0U, balancers_[2]->service_.request_count());
  EXPECT_EQ(0U, balancers_[2]->service_.response_count());

  gpr_log(GPR_INFO, "========= ABOUT TO UPDATE 1 ==========");
  SetNextResolutionForLbChannel({balancers_[1]->port_});
  gpr_log(GPR_INFO, "========= UPDATE 1 DONE ==========");

  EXPECT_EQ(0U, backends_[1]->service_.request_count());
  gpr_timespec deadline = gpr_time_add(
      gpr_now(GPR_CLOCK_REALTIME), gpr_time_from_millis(10000, GPR_TIMESPAN));
  // Send 10 seconds worth of RPCs
  do {
    CheckRpcSendOk();
  } while (gpr_time_cmp(gpr_now(GPR_CLOCK_REALTIME), deadline) < 0);
  // The current LB call is still working, so xds continued using it to the
  // first balancer, which doesn't assign the second backend.
  EXPECT_EQ(0U, backends_[1]->service_.request_count());

  EXPECT_EQ(1U, balancers_[0]->service_.request_count());
  EXPECT_EQ(1U, balancers_[0]->service_.response_count());
  EXPECT_EQ(0U, balancers_[1]->service_.request_count());
  EXPECT_EQ(0U, balancers_[1]->service_.response_count());
  EXPECT_EQ(0U, balancers_[2]->service_.request_count());
  EXPECT_EQ(0U, balancers_[2]->service_.response_count());
}

TEST_F(UpdatesTest, UpdateBalancerName) {
  SetNextResolution({}, kDefaultServiceConfig_.c_str());
  SetNextResolutionForLbChannelAllBalancers();
  const std::vector<int> first_backend{GetBackendPorts()[0]};
  const std::vector<int> second_backend{GetBackendPorts()[1]};
  ScheduleResponseForBalancer(
      0, BalancerServiceImpl::BuildResponseForBackends(first_backend, {}), 0);
  ScheduleResponseForBalancer(
      1, BalancerServiceImpl::BuildResponseForBackends(second_backend, {}), 0);

  // Wait until the first backend is ready.
  WaitForBackend(0);

  // Send 10 requests.
  gpr_log(GPR_INFO, "========= BEFORE FIRST BATCH ==========");
  CheckRpcSendOk(10);
  gpr_log(GPR_INFO, "========= DONE WITH FIRST BATCH ==========");

  // All 10 requests should have gone to the first backend.
  EXPECT_EQ(10U, backends_[0]->service_.request_count());

  // Balancer 0 got a single request.
  EXPECT_EQ(1U, balancers_[0]->service_.request_count());
  // and sent a single response.
  EXPECT_EQ(1U, balancers_[0]->service_.response_count());
  EXPECT_EQ(0U, balancers_[1]->service_.request_count());
  EXPECT_EQ(0U, balancers_[1]->service_.response_count());
  EXPECT_EQ(0U, balancers_[2]->service_.request_count());
  EXPECT_EQ(0U, balancers_[2]->service_.response_count());

  std::vector<int> ports;
  ports.emplace_back(balancers_[1]->port_);
  auto new_lb_channel_response_generator =
      grpc_core::MakeRefCounted<grpc_core::FakeResolverResponseGenerator>();
  SetNextResolutionForLbChannel(ports, nullptr,
                                new_lb_channel_response_generator.get());
  gpr_log(GPR_INFO, "========= ABOUT TO UPDATE BALANCER NAME ==========");
  SetNextResolution({},
                    "{\n"
                    "  \"loadBalancingConfig\":[\n"
                    "    { \"does_not_exist\":{} },\n"
                    "    { \"xds_experimental\":{ \"balancerName\": "
                    "\"fake:///updated_lb\" } }\n"
                    "  ]\n"
                    "}",
                    new_lb_channel_response_generator.get());
  gpr_log(GPR_INFO, "========= UPDATED BALANCER NAME ==========");

  // Wait until update has been processed, as signaled by the second backend
  // receiving a request.
  EXPECT_EQ(0U, backends_[1]->service_.request_count());
  WaitForBackend(1);

  backends_[1]->service_.ResetCounters();
  gpr_log(GPR_INFO, "========= BEFORE SECOND BATCH ==========");
  CheckRpcSendOk(10);
  gpr_log(GPR_INFO, "========= DONE WITH SECOND BATCH ==========");
  // All 10 requests should have gone to the second backend.
  EXPECT_EQ(10U, backends_[1]->service_.request_count());

  EXPECT_EQ(1U, balancers_[0]->service_.request_count());
  EXPECT_EQ(1U, balancers_[0]->service_.response_count());
  EXPECT_EQ(1U, balancers_[1]->service_.request_count());
  EXPECT_EQ(1U, balancers_[1]->service_.response_count());
  EXPECT_EQ(0U, balancers_[2]->service_.request_count());
  EXPECT_EQ(0U, balancers_[2]->service_.response_count());
}

// Send an update with the same set of LBs as the one in SetUp() in order to
// verify that the LB channel inside xds keeps the initial connection (which
// by definition is also present in the update).
TEST_F(UpdatesTest, UpdateBalancersRepeated) {
  SetNextResolution({}, kDefaultServiceConfig_.c_str());
  SetNextResolutionForLbChannelAllBalancers();
  const std::vector<int> first_backend{GetBackendPorts()[0]};
  const std::vector<int> second_backend{GetBackendPorts()[0]};

  ScheduleResponseForBalancer(
      0, BalancerServiceImpl::BuildResponseForBackends(first_backend, {}), 0);
  ScheduleResponseForBalancer(
      1, BalancerServiceImpl::BuildResponseForBackends(second_backend, {}), 0);

  // Wait until the first backend is ready.
  WaitForBackend(0);

  // Send 10 requests.
  gpr_log(GPR_INFO, "========= BEFORE FIRST BATCH ==========");
  CheckRpcSendOk(10);
  gpr_log(GPR_INFO, "========= DONE WITH FIRST BATCH ==========");

  // All 10 requests should have gone to the first backend.
  EXPECT_EQ(10U, backends_[0]->service_.request_count());

  // Balancer 0 got a single request.
  EXPECT_EQ(1U, balancers_[0]->service_.request_count());
  // and sent a single response.
  EXPECT_EQ(1U, balancers_[0]->service_.response_count());
  EXPECT_EQ(0U, balancers_[1]->service_.request_count());
  EXPECT_EQ(0U, balancers_[1]->service_.response_count());
  EXPECT_EQ(0U, balancers_[2]->service_.request_count());
  EXPECT_EQ(0U, balancers_[2]->service_.response_count());

  std::vector<int> ports;
  ports.emplace_back(balancers_[0]->port_);
  ports.emplace_back(balancers_[1]->port_);
  ports.emplace_back(balancers_[2]->port_);
  gpr_log(GPR_INFO, "========= ABOUT TO UPDATE 1 ==========");
  SetNextResolutionForLbChannel(ports);
  gpr_log(GPR_INFO, "========= UPDATE 1 DONE ==========");

  EXPECT_EQ(0U, backends_[1]->service_.request_count());
  gpr_timespec deadline = gpr_time_add(
      gpr_now(GPR_CLOCK_REALTIME), gpr_time_from_millis(10000, GPR_TIMESPAN));
  // Send 10 seconds worth of RPCs
  do {
    CheckRpcSendOk();
  } while (gpr_time_cmp(gpr_now(GPR_CLOCK_REALTIME), deadline) < 0);
  // xds continued using the original LB call to the first balancer, which
  // doesn't assign the second backend.
  EXPECT_EQ(0U, backends_[1]->service_.request_count());

  ports.clear();
  ports.emplace_back(balancers_[0]->port_);
  ports.emplace_back(balancers_[1]->port_);
  gpr_log(GPR_INFO, "========= ABOUT TO UPDATE 2 ==========");
  SetNextResolutionForLbChannel(ports);
  gpr_log(GPR_INFO, "========= UPDATE 2 DONE ==========");

  EXPECT_EQ(0U, backends_[1]->service_.request_count());
  deadline = gpr_time_add(gpr_now(GPR_CLOCK_REALTIME),
                          gpr_time_from_millis(10000, GPR_TIMESPAN));
  // Send 10 seconds worth of RPCs
  do {
    CheckRpcSendOk();
  } while (gpr_time_cmp(gpr_now(GPR_CLOCK_REALTIME), deadline) < 0);
  // xds continued using the original LB call to the first balancer, which
  // doesn't assign the second backend.
  EXPECT_EQ(0U, backends_[1]->service_.request_count());
}

TEST_F(UpdatesTest, UpdateBalancersDeadUpdate) {
  SetNextResolution({}, kDefaultServiceConfig_.c_str());
  SetNextResolutionForLbChannel({balancers_[0]->port_});
  const std::vector<int> first_backend{GetBackendPorts()[0]};
  const std::vector<int> second_backend{GetBackendPorts()[1]};

  ScheduleResponseForBalancer(
      0, BalancerServiceImpl::BuildResponseForBackends(first_backend, {}), 0);
  ScheduleResponseForBalancer(
      1, BalancerServiceImpl::BuildResponseForBackends(second_backend, {}), 0);

  // Start servers and send 10 RPCs per server.
  gpr_log(GPR_INFO, "========= BEFORE FIRST BATCH ==========");
  CheckRpcSendOk(10);
  gpr_log(GPR_INFO, "========= DONE WITH FIRST BATCH ==========");
  // All 10 requests should have gone to the first backend.
  EXPECT_EQ(10U, backends_[0]->service_.request_count());

  // Kill balancer 0
  gpr_log(GPR_INFO, "********** ABOUT TO KILL BALANCER 0 *************");
  balancers_[0]->Shutdown();
  gpr_log(GPR_INFO, "********** KILLED BALANCER 0 *************");

  // This is serviced by the existing child policy.
  gpr_log(GPR_INFO, "========= BEFORE SECOND BATCH ==========");
  CheckRpcSendOk(10);
  gpr_log(GPR_INFO, "========= DONE WITH SECOND BATCH ==========");
  // All 10 requests should again have gone to the first backend.
  EXPECT_EQ(20U, backends_[0]->service_.request_count());
  EXPECT_EQ(0U, backends_[1]->service_.request_count());

  // Balancer 0 got a single request.
  EXPECT_EQ(1U, balancers_[0]->service_.request_count());
  // and sent a single response.
  EXPECT_EQ(1U, balancers_[0]->service_.response_count());
  EXPECT_EQ(0U, balancers_[1]->service_.request_count());
  EXPECT_EQ(0U, balancers_[1]->service_.response_count());
  EXPECT_EQ(0U, balancers_[2]->service_.request_count());
  EXPECT_EQ(0U, balancers_[2]->service_.response_count());

  gpr_log(GPR_INFO, "========= ABOUT TO UPDATE 1 ==========");
  SetNextResolutionForLbChannel({balancers_[1]->port_});
  gpr_log(GPR_INFO, "========= UPDATE 1 DONE ==========");

  // Wait until update has been processed, as signaled by the second backend
  // receiving a request. In the meantime, the client continues to be serviced
  // (by the first backend) without interruption.
  EXPECT_EQ(0U, backends_[1]->service_.request_count());
  WaitForBackend(1);

  // This is serviced by the updated RR policy
  backends_[1]->service_.ResetCounters();
  gpr_log(GPR_INFO, "========= BEFORE THIRD BATCH ==========");
  CheckRpcSendOk(10);
  gpr_log(GPR_INFO, "========= DONE WITH THIRD BATCH ==========");
  // All 10 requests should have gone to the second backend.
  EXPECT_EQ(10U, backends_[1]->service_.request_count());

  EXPECT_EQ(1U, balancers_[0]->service_.request_count());
  EXPECT_EQ(1U, balancers_[0]->service_.response_count());
  // The second balancer, published as part of the first update, may end up
  // getting two requests (that is, 1 <= #req <= 2) if the LB call retry timer
  // firing races with the arrival of the update containing the second
  // balancer.
  EXPECT_GE(balancers_[1]->service_.request_count(), 1U);
  EXPECT_GE(balancers_[1]->service_.response_count(), 1U);
  EXPECT_LE(balancers_[1]->service_.request_count(), 2U);
  EXPECT_LE(balancers_[1]->service_.response_count(), 2U);
  EXPECT_EQ(0U, balancers_[2]->service_.request_count());
  EXPECT_EQ(0U, balancers_[2]->service_.response_count());
}

// The re-resolution tests are deferred because they rely on the fallback mode,
// which hasn't been supported.

// TODO(juanlishen): Add TEST_F(UpdatesTest, ReresolveDeadBackend).

// TODO(juanlishen): Add TEST_F(UpdatesWithClientLoadReportingTest,
// ReresolveDeadBalancer)

// The drop tests are deferred because the drop handling hasn't been added yet.

// TODO(roth): Add TEST_F(SingleBalancerTest, Drop)

// TODO(roth): Add TEST_F(SingleBalancerTest, DropAllFirst)

// TODO(roth): Add TEST_F(SingleBalancerTest, DropAll)

class SingleBalancerWithClientLoadReportingTest : public XdsEnd2endTest {
 public:
  SingleBalancerWithClientLoadReportingTest() : XdsEnd2endTest(4, 1, 3) {}
};

// The client load reporting tests are deferred because the client load
// reporting hasn't been supported yet.

// TODO(vpowar): Add TEST_F(SingleBalancerWithClientLoadReportingTest, Vanilla)

// TODO(roth): Add TEST_F(SingleBalancerWithClientLoadReportingTest,
// BalancerRestart)

// TODO(roth): Add TEST_F(SingleBalancerWithClientLoadReportingTest, Drop)

}  // namespace
}  // namespace testing
}  // namespace grpc

int main(int argc, char** argv) {
  grpc::testing::TestEnvironment env(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  const auto result = RUN_ALL_TESTS();
  return result;
}
