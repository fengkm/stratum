// Copyright 2018-present Open Networking Foundation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "stratum/hal/lib/dummy/dummy_sdk.h"

#include <pthread.h>

#include <algorithm>
#include <memory>
#include <utility>

#include "stratum/hal/lib/common/phal_interface.h"
#include "stratum/hal/lib/common/common.pb.h"
#include "stratum/public/proto/error.pb.h"

constexpr char kDefaultDummySDKUrl[] = "localhost:28010";
const ::absl::Duration kDefaultEventWriteTimeout = absl::Seconds(10);

DEFINE_string(dummy_test_url, kDefaultDummySDKUrl,
             "External URL for dummmy SDK server to listen to external calls.");
DEFINE_int32(dummy_test_grpc_keepalive_time_ms, 600000, "grpc keep alive time");
DEFINE_int32(dummy_test_grpc_keepalive_timeout_ms, 20000,
             "grpc keep alive timeout period");
DEFINE_int32(dummy_test_grpc_keepalive_min_ping_interval, 10000,
             "grpc keep alive minimum ping interval");
DEFINE_int32(dummy_test_grpc_keepalive_permit, 1, "grpc keep alive permit");

namespace stratum {
namespace hal {
namespace dummy_switch {

::absl::Mutex sdk_lock_;
::absl::Mutex xcvr_event_lock_;
::absl::Mutex device_event_lock_;

static DummySDK* dummy_sdk_singleton_ = nullptr;
std::unique_ptr<::grpc::Server> external_server_;

void* ExternalServerWaitingFunc(void* arg) {
  if (external_server_ == nullptr) {
    LOG(ERROR) << "gRPC server does not initialized";
    return nullptr;
  }
  LOG(INFO) << "Listen test service on " << FLAGS_dummy_test_url << ".";
  external_server_->Wait();  // block
  return nullptr;
}

::grpc::Status
DummySDK::DeviceStatusUpdate(::grpc::ServerContext* context,
                             const DeviceStatusUpdateRequest* request,
                             DeviceStatusUpdateResponse* response) {
  // The response is always an empty message
  response = new DeviceStatusUpdateResponse();
  switch (request->source().source_case()) {
    case DeviceStatusUpdateRequest::Source::kPort:
      return HandlePortStatusUpdate(request->source().port().node_id(),
                                    request->source().port().port_id(),
                                    request->state_update());
    case DeviceStatusUpdateRequest::Source::kNode:
    case DeviceStatusUpdateRequest::Source::kPortQueue:
    case DeviceStatusUpdateRequest::Source::kChassis:
    default:
      return ::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED,
                            "Not implement yet!");
  }
}

::grpc::Status
DummySDK::TransceiverEventUpdate(::grpc::ServerContext* context,
                 const TransceiverEventRequest* request,
                 TransceiverEventResponse* response) {
  response = new TransceiverEventResponse();
  for (auto& writer_elem : xcvr_event_writers_) {
    PhalInterface::TransceiverEvent event;
    event.slot = request->slot();
    event.port = request->port();
    event.state = request->state();
    writer_elem.writer->Write(event, kDefaultEventWriteTimeout);
  }
  return ::grpc::Status();
}

::grpc::Status
DummySDK::HandlePortStatusUpdate(uint64 node_id,
                                 uint64 port_id,
                                 ::stratum::hal::DataResponse state_update) {
  auto event_writer_elem = node_event_notify_writers_.find(node_id);
  if (event_writer_elem == node_event_notify_writers_.end()) {
    // No event writer for this device can handle the event.
    LOG(WARNING) << "Receives device status update event, however"
      << " there is no event writer for device id " << node_id
      << " found, drop event.";
    return ::grpc::Status(::grpc::StatusCode::NOT_FOUND,
                          "Event writer not found");
  }
  auto node_event_notify_writer_ = event_writer_elem->second;

  DummyNodeEvent* event = new DummyNodeEvent();
  event->node_id = node_id;
  event->port_id = port_id;
  event->state_update = state_update;
  node_event_notify_writer_->Write(DummyNodeEventPtr(event));
  return ::grpc::Status();
}

::util::StatusOr<int>
DummySDK::RegisterTransceiverEventWriter(
  std::unique_ptr<ChannelWriter<PhalInterface::TransceiverEvent>> writer,
  int priority) {
  // Generate new transceiver writer ID
  ++xcvr_writer_id_;
  PhalInterface::TransceiverEventWriter xcvr_event_writer;
  xcvr_event_writer.writer = std::move(writer);
  xcvr_event_writer.priority = priority;
  xcvr_event_writer.id = xcvr_writer_id_;
  xcvr_event_writers_.push_back(std::move(xcvr_event_writer));

  std::sort(xcvr_event_writers_.begin(),
            xcvr_event_writers_.end(),
            TransceiverEventWriterComp());
  return ::util::StatusOr<int>(xcvr_writer_id_);
}

::util::Status DummySDK::UnregisterTransceiverEventWriter(int id) {
  std::remove_if(xcvr_event_writers_.begin(),
                 xcvr_event_writers_.end(),
                 FindXcvrById(id));
  return ::util::OkStatus();
}

::util::Status
DummySDK::RegisterNodeEventNotifyWriter(
  uint64 node_id,
  std::shared_ptr<WriterInterface<DummyNodeEventPtr>> writer) {
  if (node_event_notify_writers_.find(node_id) !=
        node_event_notify_writers_.end()) {
    return ::util::Status(::util::error::ALREADY_EXISTS,
                          "Writer already exists");
  }

  node_event_notify_writers_.emplace(node_id, writer);
  return ::util::OkStatus();
}

::util::Status DummySDK::UnregisterNodeEventNotifyWriter(uint64 node_id) {
  if (node_event_notify_writers_.find(node_id) ==
        node_event_notify_writers_.end()) {
    return ::util::Status(::util::error::NOT_FOUND,
                          "Writer not found");
  }
  node_event_notify_writers_.erase(node_id);
  return ::util::OkStatus();
}

::util::Status DummySDK::RegisterChassisEventNotifyWriter(
      std::shared_ptr<WriterInterface<GnmiEventPtr>> writer) {
  if (writer) {
    return MAKE_ERROR(ERR_INTERNAL) << "Chassis event writer already exists";
  }
  chassis_event_notify_writer_ = writer;
  return ::util::OkStatus();
}

::util::Status DummySDK::UnregisterChassisEventNotifyWriter() {
  chassis_event_notify_writer_.reset();
  return ::util::OkStatus();
}

DummySDK* DummySDK::GetSingleton() {
  if (dummy_sdk_singleton_ == nullptr) {
    dummy_sdk_singleton_ = new DummySDK();
  }
  return dummy_sdk_singleton_;
}

::util::Status DummySDK::Start() {
  if (initialized_) {
    return MAKE_ERROR(ERR_ABORTED) << "SDK already initialized";
  }

  // Initialize the gRPC server with test service.
  ::grpc::ServerBuilder builder;
  builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIME_MS,
                             FLAGS_dummy_test_grpc_keepalive_time_ms);
  builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIMEOUT_MS,
                             FLAGS_dummy_test_grpc_keepalive_timeout_ms);
  builder.AddChannelArgument(
      GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS,
      FLAGS_dummy_test_grpc_keepalive_min_ping_interval);
  builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS,
                             FLAGS_dummy_test_grpc_keepalive_permit);
  builder.AddListeningPort(FLAGS_dummy_test_url,
                           ::grpc::InsecureServerCredentials());
  builder.RegisterService(this);

  external_server_ = builder.BuildAndStart();
  if (external_server_ == nullptr) {
    return MAKE_ERROR(ERR_INTERNAL)
            << "Failed to start DummySDK test service to listen "
            << "to " << FLAGS_dummy_test_url << ".";
  }

  // Create another thread to run "external_server_->Wait()" since we can not
  // block the main thread here
  pthread_t external_server_thread_;
  int ret = pthread_create(&external_server_thread_, nullptr,
                           ExternalServerWaitingFunc, nullptr);

  if (ret != 0) {
    return MAKE_ERROR(ERR_INTERNAL)
            << "Failed to create server listen thread. Err: " << ret << ".";
  }

  initialized_ = true;
  return ::util::OkStatus();
}

::util::Status DummySDK::Shutdown() {
  LOG(INFO) << "Shutting down the DummySDK.";
  external_server_->Shutdown(std::chrono::system_clock::now());
  return ::util::OkStatus();
}

DummySDK::~DummySDK() {}
DummySDK::DummySDK()
: initialized_(false),
  xcvr_writer_id_(0) {}

}  // namespace dummy_switch
}  // namespace hal
}  // namespace stratum