/*
 *
 * Copyright 2016 gRPC authors.
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

#include <algorithm>
#include <memory>
#include <mutex>
#include <thread>

#include <grpc++/channel.h>
#include <grpc++/client_context.h>
#include <grpc++/create_channel.h>
#include <grpc++/server.h>
#include <grpc++/server_builder.h>
#include <grpc/grpc.h>
#include <grpc/support/alloc.h>
#include <grpc/support/atm.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <grpc/support/thd.h>
#include <grpc/support/time.h>

#include "src/core/ext/filters/client_channel/resolver/fake/fake_resolver.h"
#include "src/core/ext/filters/client_channel/subchannel_index.h"
#include "src/core/lib/backoff/backoff.h"
#include "src/core/lib/gpr/env.h"

#include "src/proto/grpc/testing/echo.grpc.pb.h"
#include "test/core/util/port.h"
#include "test/core/util/test_config.h"
#include "test/cpp/end2end/test_service_impl.h"

#include <gtest/gtest.h>

using grpc::testing::EchoRequest;
using grpc::testing::EchoResponse;
using std::chrono::system_clock;

// defined in tcp_client_posix.c
extern void (*grpc_tcp_client_connect_impl)(
    grpc_closure* closure, grpc_endpoint** ep,
    grpc_pollset_set* interested_parties, const grpc_channel_args* channel_args,
    const grpc_resolved_address* addr, grpc_millis deadline);

const auto original_tcp_connect_fn = grpc_tcp_client_connect_impl;

namespace grpc {
namespace testing {
namespace {

gpr_atm g_connection_delay_ms;

void tcp_client_connect_with_delay(grpc_closure* closure, grpc_endpoint** ep,
                                   grpc_pollset_set* interested_parties,
                                   const grpc_channel_args* channel_args,
                                   const grpc_resolved_address* addr,
                                   grpc_millis deadline) {
  const int delay_ms = gpr_atm_acq_load(&g_connection_delay_ms);
  if (delay_ms > 0) {
    gpr_sleep_until(grpc_timeout_milliseconds_to_deadline(delay_ms));
  }
  original_tcp_connect_fn(closure, ep, interested_parties, channel_args, addr,
                          deadline + delay_ms);
}

// Subclass of TestServiceImpl that increments a request counter for
// every call to the Echo RPC.
class MyTestServiceImpl : public TestServiceImpl {
 public:
  MyTestServiceImpl() : request_count_(0) {}

  Status Echo(ServerContext* context, const EchoRequest* request,
              EchoResponse* response) override {
    {
      std::unique_lock<std::mutex> lock(mu_);
      ++request_count_;
    }
    return TestServiceImpl::Echo(context, request, response);
  }

  int request_count() {
    std::unique_lock<std::mutex> lock(mu_);
    return request_count_;
  }

  void ResetCounters() {
    std::unique_lock<std::mutex> lock(mu_);
    request_count_ = 0;
  }

 private:
  std::mutex mu_;
  int request_count_;
};

class ClientLbEnd2endTest : public ::testing::Test {
 protected:
  ClientLbEnd2endTest()
      : server_host_("localhost"), kRequestMessage_("Live long and prosper.") {
    // Make the backup poller poll very frequently in order to pick up
    // updates from all the subchannels's FDs.
    gpr_setenv("GRPC_CLIENT_CHANNEL_BACKUP_POLL_INTERVAL_MS", "1");
  }

  void SetUp() override {
    response_generator_ = grpc_fake_resolver_response_generator_create();
  }

  void TearDown() override {
    grpc_fake_resolver_response_generator_unref(response_generator_);
    for (size_t i = 0; i < servers_.size(); ++i) {
      servers_[i]->Shutdown();
    }
  }

  void StartServers(size_t num_servers,
                    std::vector<int> ports = std::vector<int>()) {
    for (size_t i = 0; i < num_servers; ++i) {
      int port = 0;
      if (ports.size() == num_servers) port = ports[i];
      servers_.emplace_back(new ServerData(server_host_, port));
    }
  }

  void SetNextResolution(const std::vector<int>& ports) {
    grpc_core::ExecCtx exec_ctx;
    grpc_lb_addresses* addresses =
        grpc_lb_addresses_create(ports.size(), nullptr);
    for (size_t i = 0; i < ports.size(); ++i) {
      char* lb_uri_str;
      gpr_asprintf(&lb_uri_str, "ipv4:127.0.0.1:%d", ports[i]);
      grpc_uri* lb_uri = grpc_uri_parse(lb_uri_str, true);
      GPR_ASSERT(lb_uri != nullptr);
      grpc_lb_addresses_set_address_from_uri(addresses, i, lb_uri,
                                             false /* is balancer */,
                                             "" /* balancer name */, nullptr);
      grpc_uri_destroy(lb_uri);
      gpr_free(lb_uri_str);
    }
    const grpc_arg fake_addresses =
        grpc_lb_addresses_create_channel_arg(addresses);
    grpc_channel_args* fake_result =
        grpc_channel_args_copy_and_add(nullptr, &fake_addresses, 1);
    grpc_fake_resolver_response_generator_set_response(response_generator_,
                                                       fake_result);
    grpc_channel_args_destroy(fake_result);
    grpc_lb_addresses_destroy(addresses);
  }

  std::vector<int> GetServersPorts() {
    std::vector<int> ports;
    for (const auto& server : servers_) ports.push_back(server->port_);
    return ports;
  }

  void ResetStub(const std::vector<int>& ports,
                 const grpc::string& lb_policy_name,
                 ChannelArguments args = ChannelArguments()) {
    if (lb_policy_name.size() > 0) {
      args.SetLoadBalancingPolicyName(lb_policy_name);
    }  // else, default to pick first
    args.SetPointer(GRPC_ARG_FAKE_RESOLVER_RESPONSE_GENERATOR,
                    response_generator_);
    channel_ =
        CreateCustomChannel("fake:///", InsecureChannelCredentials(), args);
    stub_ = grpc::testing::EchoTestService::NewStub(channel_);
  }

  bool SendRpc(EchoResponse* response = nullptr) {
    const bool local_response = (response == nullptr);
    if (local_response) response = new EchoResponse;
    EchoRequest request;
    request.set_message(kRequestMessage_);
    ClientContext context;
    Status status = stub_->Echo(&context, request, response);
    if (local_response) delete response;
    return status.ok();
  }

  void CheckRpcSendOk() {
    EchoResponse response;
    const bool success = SendRpc(&response);
    EXPECT_TRUE(success);
    EXPECT_EQ(response.message(), kRequestMessage_);
  }

  void CheckRpcSendFailure() {
    const bool success = SendRpc();
    EXPECT_FALSE(success);
  }

  struct ServerData {
    int port_;
    std::unique_ptr<Server> server_;
    MyTestServiceImpl service_;
    std::unique_ptr<std::thread> thread_;
    bool server_ready_ = false;

    explicit ServerData(const grpc::string& server_host, int port = 0) {
      port_ = port > 0 ? port : grpc_pick_unused_port_or_die();
      gpr_log(GPR_INFO, "starting server on port %d", port_);
      std::mutex mu;
      std::unique_lock<std::mutex> lock(mu);
      std::condition_variable cond;
      thread_.reset(new std::thread(
          std::bind(&ServerData::Start, this, server_host, &mu, &cond)));
      cond.wait(lock, [this] { return server_ready_; });
      server_ready_ = false;
      gpr_log(GPR_INFO, "server startup complete");
    }

    void Start(const grpc::string& server_host, std::mutex* mu,
               std::condition_variable* cond) {
      std::ostringstream server_address;
      server_address << server_host << ":" << port_;
      ServerBuilder builder;
      builder.AddListeningPort(server_address.str(),
                               InsecureServerCredentials());
      builder.RegisterService(&service_);
      server_ = builder.BuildAndStart();
      std::lock_guard<std::mutex> lock(*mu);
      server_ready_ = true;
      cond->notify_one();
    }

    void Shutdown(bool join = true) {
      server_->Shutdown();
      if (join) thread_->join();
    }
  };

  void ResetCounters() {
    for (const auto& server : servers_) server->service_.ResetCounters();
  }

  void WaitForServer(size_t server_idx) {
    do {
      CheckRpcSendOk();
    } while (servers_[server_idx]->service_.request_count() == 0);
    ResetCounters();
  }

  bool SeenAllServers() {
    for (const auto& server : servers_) {
      if (server->service_.request_count() == 0) return false;
    }
    return true;
  }

  // Updates \a connection_order by appending to it the index of the newly
  // connected server. Must be called after every single RPC.
  void UpdateConnectionOrder(
      const std::vector<std::unique_ptr<ServerData>>& servers,
      std::vector<int>* connection_order) {
    for (size_t i = 0; i < servers.size(); ++i) {
      if (servers[i]->service_.request_count() == 1) {
        // Was the server index known? If not, update connection_order.
        const auto it =
            std::find(connection_order->begin(), connection_order->end(), i);
        if (it == connection_order->end()) {
          connection_order->push_back(i);
          return;
        }
      }
    }
  }

  const grpc::string server_host_;
  std::shared_ptr<Channel> channel_;
  std::unique_ptr<grpc::testing::EchoTestService::Stub> stub_;
  std::vector<std::unique_ptr<ServerData>> servers_;
  grpc_fake_resolver_response_generator* response_generator_;
  const grpc::string kRequestMessage_;
};

TEST_F(ClientLbEnd2endTest, PickFirst) {
  // Start servers and send one RPC per server.
  const int kNumServers = 3;
  StartServers(kNumServers);
  ResetStub(GetServersPorts(), "");  // test that pick first is the default.
  std::vector<int> ports;
  for (size_t i = 0; i < servers_.size(); ++i) {
    ports.emplace_back(servers_[i]->port_);
  }
  SetNextResolution(ports);
  for (size_t i = 0; i < servers_.size(); ++i) {
    CheckRpcSendOk();
  }
  // All requests should have gone to a single server.
  bool found = false;
  for (size_t i = 0; i < servers_.size(); ++i) {
    const int request_count = servers_[i]->service_.request_count();
    if (request_count == kNumServers) {
      found = true;
    } else {
      EXPECT_EQ(0, request_count);
    }
  }
  EXPECT_TRUE(found);
  // Check LB policy name for the channel.
  EXPECT_EQ("pick_first", channel_->GetLoadBalancingPolicyName());
}

TEST_F(ClientLbEnd2endTest, PickFirstBackOffInitialReconnect) {
  ChannelArguments args;
  constexpr int kInitialBackOffMs = 100;
  args.SetInt(GRPC_ARG_INITIAL_RECONNECT_BACKOFF_MS, kInitialBackOffMs);
  const std::vector<int> ports = {grpc_pick_unused_port_or_die()};
  const gpr_timespec t0 = gpr_now(GPR_CLOCK_MONOTONIC);
  ResetStub(ports, "pick_first", args);
  SetNextResolution(ports);
  // The channel won't become connected (there's no server).
  ASSERT_FALSE(channel_->WaitForConnected(
      grpc_timeout_milliseconds_to_deadline(kInitialBackOffMs * 2)));
  // Bring up a server on the chosen port.
  StartServers(1, ports);
  // Now it will.
  ASSERT_TRUE(channel_->WaitForConnected(
      grpc_timeout_milliseconds_to_deadline(kInitialBackOffMs * 2)));
  const gpr_timespec t1 = gpr_now(GPR_CLOCK_MONOTONIC);
  const grpc_millis waited_ms = gpr_time_to_millis(gpr_time_sub(t1, t0));
  gpr_log(GPR_DEBUG, "Waited %ld milliseconds", waited_ms);
  // We should have waited at least kInitialBackOffMs. We substract one to
  // account for test and precision accuracy drift.
  EXPECT_GE(waited_ms, kInitialBackOffMs - 1);
  // But not much more.
  EXPECT_GT(
      gpr_time_cmp(
          grpc_timeout_milliseconds_to_deadline(kInitialBackOffMs * 1.10), t1),
      0);
}

TEST_F(ClientLbEnd2endTest, PickFirstBackOffMinReconnect) {
  ChannelArguments args;
  constexpr int kMinReconnectBackOffMs = 1000;
  args.SetInt(GRPC_ARG_MIN_RECONNECT_BACKOFF_MS, kMinReconnectBackOffMs);
  const std::vector<int> ports = {grpc_pick_unused_port_or_die()};
  ResetStub(ports, "pick_first", args);
  SetNextResolution(ports);
  // Make connection delay a 10% longer than it's willing to in order to make
  // sure we are hitting the codepath that waits for the min reconnect backoff.
  gpr_atm_rel_store(&g_connection_delay_ms, kMinReconnectBackOffMs * 1.10);
  grpc_tcp_client_connect_impl = tcp_client_connect_with_delay;
  const gpr_timespec t0 = gpr_now(GPR_CLOCK_MONOTONIC);
  channel_->WaitForConnected(
      grpc_timeout_milliseconds_to_deadline(kMinReconnectBackOffMs * 2));
  const gpr_timespec t1 = gpr_now(GPR_CLOCK_MONOTONIC);
  const grpc_millis waited_ms = gpr_time_to_millis(gpr_time_sub(t1, t0));
  gpr_log(GPR_DEBUG, "Waited %ld ms", waited_ms);
  // We should have waited at least kMinReconnectBackOffMs. We substract one to
  // account for test and precision accuracy drift.
  EXPECT_GE(waited_ms, kMinReconnectBackOffMs - 1);
  gpr_atm_rel_store(&g_connection_delay_ms, 0);
}

TEST_F(ClientLbEnd2endTest, PickFirstUpdates) {
  // Start servers and send one RPC per server.
  const int kNumServers = 3;
  StartServers(kNumServers);
  ResetStub(GetServersPorts(), "pick_first");
  std::vector<int> ports;

  // Perform one RPC against the first server.
  ports.emplace_back(servers_[0]->port_);
  SetNextResolution(ports);
  gpr_log(GPR_INFO, "****** SET [0] *******");
  CheckRpcSendOk();
  EXPECT_EQ(servers_[0]->service_.request_count(), 1);

  // An empty update will result in the channel going into TRANSIENT_FAILURE.
  ports.clear();
  SetNextResolution(ports);
  gpr_log(GPR_INFO, "****** SET none *******");
  grpc_connectivity_state channel_state;
  do {
    channel_state = channel_->GetState(true /* try to connect */);
  } while (channel_state == GRPC_CHANNEL_READY);
  GPR_ASSERT(channel_state != GRPC_CHANNEL_READY);
  servers_[0]->service_.ResetCounters();

  // Next update introduces servers_[1], making the channel recover.
  ports.clear();
  ports.emplace_back(servers_[1]->port_);
  SetNextResolution(ports);
  gpr_log(GPR_INFO, "****** SET [1] *******");
  WaitForServer(1);
  EXPECT_EQ(servers_[0]->service_.request_count(), 0);

  // And again for servers_[2]
  ports.clear();
  ports.emplace_back(servers_[2]->port_);
  SetNextResolution(ports);
  gpr_log(GPR_INFO, "****** SET [2] *******");
  WaitForServer(2);
  EXPECT_EQ(servers_[0]->service_.request_count(), 0);
  EXPECT_EQ(servers_[1]->service_.request_count(), 0);

  // Check LB policy name for the channel.
  EXPECT_EQ("pick_first", channel_->GetLoadBalancingPolicyName());
}

TEST_F(ClientLbEnd2endTest, PickFirstUpdateSuperset) {
  // Start servers and send one RPC per server.
  const int kNumServers = 3;
  StartServers(kNumServers);
  ResetStub(GetServersPorts(), "pick_first");
  std::vector<int> ports;

  // Perform one RPC against the first server.
  ports.emplace_back(servers_[0]->port_);
  SetNextResolution(ports);
  gpr_log(GPR_INFO, "****** SET [0] *******");
  CheckRpcSendOk();
  EXPECT_EQ(servers_[0]->service_.request_count(), 1);
  servers_[0]->service_.ResetCounters();

  // Send and superset update
  ports.clear();
  ports.emplace_back(servers_[1]->port_);
  ports.emplace_back(servers_[0]->port_);
  SetNextResolution(ports);
  gpr_log(GPR_INFO, "****** SET superset *******");
  CheckRpcSendOk();
  // We stick to the previously connected server.
  WaitForServer(0);
  EXPECT_EQ(0, servers_[1]->service_.request_count());

  // Check LB policy name for the channel.
  EXPECT_EQ("pick_first", channel_->GetLoadBalancingPolicyName());
}

TEST_F(ClientLbEnd2endTest, PickFirstManyUpdates) {
  // Start servers and send one RPC per server.
  const int kNumServers = 3;
  StartServers(kNumServers);
  ResetStub(GetServersPorts(), "pick_first");
  std::vector<int> ports;
  for (size_t i = 0; i < servers_.size(); ++i) {
    ports.emplace_back(servers_[i]->port_);
  }
  for (const bool force_creation : {true, false}) {
    grpc_subchannel_index_test_only_set_force_creation(force_creation);
    gpr_log(GPR_INFO, "Force subchannel creation: %d", force_creation);
    for (size_t i = 0; i < 1000; ++i) {
      std::random_shuffle(ports.begin(), ports.end());
      SetNextResolution(ports);
      if (i % 10 == 0) CheckRpcSendOk();
    }
  }
  // Check LB policy name for the channel.
  EXPECT_EQ("pick_first", channel_->GetLoadBalancingPolicyName());
}

TEST_F(ClientLbEnd2endTest, RoundRobin) {
  // Start servers and send one RPC per server.
  const int kNumServers = 3;
  StartServers(kNumServers);
  ResetStub(GetServersPorts(), "round_robin");
  std::vector<int> ports;
  for (const auto& server : servers_) {
    ports.emplace_back(server->port_);
  }
  SetNextResolution(ports);
  // Wait until all backends are ready.
  do {
    CheckRpcSendOk();
  } while (!SeenAllServers());
  ResetCounters();
  // "Sync" to the end of the list. Next sequence of picks will start at the
  // first server (index 0).
  WaitForServer(servers_.size() - 1);
  std::vector<int> connection_order;
  for (size_t i = 0; i < servers_.size(); ++i) {
    CheckRpcSendOk();
    UpdateConnectionOrder(servers_, &connection_order);
  }
  // Backends should be iterated over in the order in which the addresses were
  // given.
  const auto expected = std::vector<int>{0, 1, 2};
  EXPECT_EQ(expected, connection_order);
  // Check LB policy name for the channel.
  EXPECT_EQ("round_robin", channel_->GetLoadBalancingPolicyName());
}

TEST_F(ClientLbEnd2endTest, RoundRobinUpdates) {
  // Start servers and send one RPC per server.
  const int kNumServers = 3;
  StartServers(kNumServers);
  ResetStub(GetServersPorts(), "round_robin");
  std::vector<int> ports;

  // Start with a single server.
  ports.emplace_back(servers_[0]->port_);
  SetNextResolution(ports);
  WaitForServer(0);
  // Send RPCs. They should all go servers_[0]
  for (size_t i = 0; i < 10; ++i) CheckRpcSendOk();
  EXPECT_EQ(10, servers_[0]->service_.request_count());
  EXPECT_EQ(0, servers_[1]->service_.request_count());
  EXPECT_EQ(0, servers_[2]->service_.request_count());
  servers_[0]->service_.ResetCounters();

  // And now for the second server.
  ports.clear();
  ports.emplace_back(servers_[1]->port_);
  SetNextResolution(ports);

  // Wait until update has been processed, as signaled by the second backend
  // receiving a request.
  EXPECT_EQ(0, servers_[1]->service_.request_count());
  WaitForServer(1);

  for (size_t i = 0; i < 10; ++i) CheckRpcSendOk();
  EXPECT_EQ(0, servers_[0]->service_.request_count());
  EXPECT_EQ(10, servers_[1]->service_.request_count());
  EXPECT_EQ(0, servers_[2]->service_.request_count());
  servers_[1]->service_.ResetCounters();

  // ... and for the last server.
  ports.clear();
  ports.emplace_back(servers_[2]->port_);
  SetNextResolution(ports);
  WaitForServer(2);

  for (size_t i = 0; i < 10; ++i) CheckRpcSendOk();
  EXPECT_EQ(0, servers_[0]->service_.request_count());
  EXPECT_EQ(0, servers_[1]->service_.request_count());
  EXPECT_EQ(10, servers_[2]->service_.request_count());
  servers_[2]->service_.ResetCounters();

  // Back to all servers.
  ports.clear();
  ports.emplace_back(servers_[0]->port_);
  ports.emplace_back(servers_[1]->port_);
  ports.emplace_back(servers_[2]->port_);
  SetNextResolution(ports);
  WaitForServer(0);
  WaitForServer(1);
  WaitForServer(2);

  // Send three RPCs, one per server.
  for (size_t i = 0; i < 3; ++i) CheckRpcSendOk();
  EXPECT_EQ(1, servers_[0]->service_.request_count());
  EXPECT_EQ(1, servers_[1]->service_.request_count());
  EXPECT_EQ(1, servers_[2]->service_.request_count());

  // An empty update will result in the channel going into TRANSIENT_FAILURE.
  ports.clear();
  SetNextResolution(ports);
  grpc_connectivity_state channel_state;
  do {
    channel_state = channel_->GetState(true /* try to connect */);
  } while (channel_state == GRPC_CHANNEL_READY);
  GPR_ASSERT(channel_state != GRPC_CHANNEL_READY);
  servers_[0]->service_.ResetCounters();

  // Next update introduces servers_[1], making the channel recover.
  ports.clear();
  ports.emplace_back(servers_[1]->port_);
  SetNextResolution(ports);
  WaitForServer(1);
  channel_state = channel_->GetState(false /* try to connect */);
  GPR_ASSERT(channel_state == GRPC_CHANNEL_READY);

  // Check LB policy name for the channel.
  EXPECT_EQ("round_robin", channel_->GetLoadBalancingPolicyName());
}

TEST_F(ClientLbEnd2endTest, RoundRobinUpdateInError) {
  const int kNumServers = 3;
  StartServers(kNumServers);
  ResetStub(GetServersPorts(), "round_robin");
  std::vector<int> ports;

  // Start with a single server.
  ports.emplace_back(servers_[0]->port_);
  SetNextResolution(ports);
  WaitForServer(0);
  // Send RPCs. They should all go to servers_[0]
  for (size_t i = 0; i < 10; ++i) SendRpc();
  EXPECT_EQ(10, servers_[0]->service_.request_count());
  EXPECT_EQ(0, servers_[1]->service_.request_count());
  EXPECT_EQ(0, servers_[2]->service_.request_count());
  servers_[0]->service_.ResetCounters();

  // Shutdown one of the servers to be sent in the update.
  servers_[1]->Shutdown(false);
  ports.emplace_back(servers_[1]->port_);
  ports.emplace_back(servers_[2]->port_);
  SetNextResolution(ports);
  WaitForServer(0);
  WaitForServer(2);

  // Send three RPCs, one per server.
  for (size_t i = 0; i < kNumServers; ++i) SendRpc();
  // The server in shutdown shouldn't receive any.
  EXPECT_EQ(0, servers_[1]->service_.request_count());
}

TEST_F(ClientLbEnd2endTest, RoundRobinManyUpdates) {
  // Start servers and send one RPC per server.
  const int kNumServers = 3;
  StartServers(kNumServers);
  ResetStub(GetServersPorts(), "round_robin");
  std::vector<int> ports;
  for (size_t i = 0; i < servers_.size(); ++i) {
    ports.emplace_back(servers_[i]->port_);
  }
  for (size_t i = 0; i < 1000; ++i) {
    std::random_shuffle(ports.begin(), ports.end());
    SetNextResolution(ports);
    if (i % 10 == 0) CheckRpcSendOk();
  }
  // Check LB policy name for the channel.
  EXPECT_EQ("round_robin", channel_->GetLoadBalancingPolicyName());
}

TEST_F(ClientLbEnd2endTest, RoundRobinConcurrentUpdates) {
  // TODO(dgq): replicate the way internal testing exercises the concurrent
  // update provisions of RR.
}

TEST_F(ClientLbEnd2endTest, RoundRobinReresolve) {
  // Start servers and send one RPC per server.
  const int kNumServers = 3;
  std::vector<int> ports;
  for (int i = 0; i < kNumServers; ++i) {
    ports.push_back(grpc_pick_unused_port_or_die());
  }
  StartServers(kNumServers, ports);
  ResetStub(GetServersPorts(), "round_robin");
  SetNextResolution(ports);
  // Send a number of RPCs, which succeed.
  for (size_t i = 0; i < 100; ++i) {
    CheckRpcSendOk();
  }
  // Kill all servers
  gpr_log(GPR_INFO, "****** ABOUT TO KILL SERVERS *******");
  for (size_t i = 0; i < servers_.size(); ++i) {
    servers_[i]->Shutdown(false);
  }
  gpr_log(GPR_INFO, "****** SERVERS KILLED *******");
  gpr_log(GPR_INFO, "****** SENDING DOOMED REQUESTS *******");
  // Client requests should fail. Send enough to tickle all subchannels.
  for (size_t i = 0; i < servers_.size(); ++i) CheckRpcSendFailure();
  gpr_log(GPR_INFO, "****** DOOMED REQUESTS SENT *******");
  // Bring servers back up on the same port (we aren't recreating the channel).
  gpr_log(GPR_INFO, "****** RESTARTING SERVERS *******");
  StartServers(kNumServers, ports);
  gpr_log(GPR_INFO, "****** SERVERS RESTARTED *******");
  gpr_log(GPR_INFO, "****** SENDING REQUEST TO SUCCEED *******");
  // Client request should eventually (but still fairly soon) succeed.
  const gpr_timespec deadline = grpc_timeout_seconds_to_deadline(5);
  gpr_timespec now = gpr_now(GPR_CLOCK_MONOTONIC);
  while (gpr_time_cmp(deadline, now) > 0) {
    if (SendRpc()) break;
    now = gpr_now(GPR_CLOCK_MONOTONIC);
  }
  GPR_ASSERT(gpr_time_cmp(deadline, now) > 0);
}

TEST_F(ClientLbEnd2endTest, RoundRobinSingleReconnect) {
  const int kNumServers = 3;
  StartServers(kNumServers);
  const auto ports = GetServersPorts();
  ResetStub(ports, "round_robin");
  SetNextResolution(ports);
  for (size_t i = 0; i < kNumServers; ++i) WaitForServer(i);
  for (size_t i = 0; i < servers_.size(); ++i) {
    CheckRpcSendOk();
    EXPECT_EQ(1, servers_[i]->service_.request_count()) << "for backend #" << i;
  }
  // One request should have gone to each server.
  for (size_t i = 0; i < servers_.size(); ++i) {
    EXPECT_EQ(1, servers_[i]->service_.request_count());
  }
  const auto pre_death = servers_[0]->service_.request_count();
  // Kill the first server.
  servers_[0]->Shutdown(true);
  // Client request still succeed. May need retrying if RR had returned a pick
  // before noticing the change in the server's connectivity.
  while (!SendRpc())
    ;  // Retry until success.
  // Send a bunch of RPCs that should succeed.
  for (int i = 0; i < 10 * kNumServers; ++i) CheckRpcSendOk();
  const auto post_death = servers_[0]->service_.request_count();
  // No requests have gone to the deceased server.
  EXPECT_EQ(pre_death, post_death);
  // Bring the first server back up.
  servers_[0].reset(new ServerData(server_host_, ports[0]));
  // Requests should start arriving at the first server either right away (if
  // the server managed to start before the RR policy retried the subchannel) or
  // after the subchannel retry delay otherwise (RR's subchannel retried before
  // the server was fully back up).
  WaitForServer(0);
}

}  // namespace
}  // namespace testing
}  // namespace grpc

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  grpc_test_init(argc, argv);
  grpc_init();
  const auto result = RUN_ALL_TESTS();
  grpc_shutdown();
  return result;
}
