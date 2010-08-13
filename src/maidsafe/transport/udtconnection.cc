﻿/* Copyright (c) 2010 maidsafe.net limited
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
    this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice,
    this list of conditions and the following disclaimer in the documentation
    and/or other materials provided with the distribution.
    * Neither the name of the maidsafe.net limited nor the names of its
    contributors may be used to endorse or promote products derived from this
    software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "maidsafe/transport/udtconnection.h"
#include <boost/cstdint.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/scoped_array.hpp>
#include <google/protobuf/descriptor.h>
#include "maidsafe/base/log.h"
#include "maidsafe/base/threadpool.h"
#include "maidsafe/transport/udttransport.h"

namespace transport {

UdtConnection::UdtConnection(const IP &remote_ip,
                             const Port &remote_port,
                             const IP &rendezvous_ip,
                             const Port &rendezvous_port)
    : udt_transport_(NULL),
      signals_(new Signals),
      threadpool_(),
      worker_(),
      udt_socket_id_(UDT::INVALID_SOCK),
      remote_ip_(remote_ip),
      remote_port_(remote_port),
      rendezvous_ip_(rendezvous_ip),
      rendezvous_port_(rendezvous_port),
      peer_(),
      transport_message_(),
      send_timeout_(kDefaultSendTimeout),
      receive_timeout_(kDefaultReceiveTimeout) {
  Init();
}

UdtConnection::UdtConnection(UdtTransport *udt_transport,
                             const IP &remote_ip,
                             const Port &remote_port,
                             const IP &rendezvous_ip,
                             const Port &rendezvous_port)
    : udt_transport_(udt_transport),
      signals_(udt_transport->signals()),
      threadpool_(udt_transport->general_threadpool_),
      worker_(),
      udt_socket_id_(UDT::INVALID_SOCK),
      remote_ip_(remote_ip),
      remote_port_(remote_port),
      rendezvous_ip_(rendezvous_ip),
      rendezvous_port_(rendezvous_port),
      peer_(),
      transport_message_(),
      send_timeout_(kDefaultSendTimeout),
      receive_timeout_(kDefaultReceiveTimeout) {
  Init();
}

UdtConnection::UdtConnection(UdtTransport *udt_transport,
                             const UdtSocketId &udt_socket_id)
    : udt_transport_(udt_transport),
      signals_(udt_transport->signals()),
      threadpool_(udt_transport->general_threadpool_),
      worker_(),
      udt_socket_id_(udt_socket_id),
      remote_ip_(""),
      remote_port_(0),
      rendezvous_ip_(""),
      rendezvous_port_(0),
      peer_(),
      transport_message_(),
      send_timeout_(kDefaultSendTimeout),
      receive_timeout_(kDefaultReceiveTimeout) {}

UdtConnection::UdtConnection(const UdtConnection &other)
    : udt_transport_(other.udt_transport_),
      signals_(other.signals_),
      threadpool_(other.threadpool_),
      worker_(other.worker_),
      udt_socket_id_(other.udt_socket_id_),
      remote_ip_(other.remote_ip_),
      remote_port_(other.remote_port_),
      rendezvous_ip_(other.rendezvous_ip_),
      rendezvous_port_(other.rendezvous_port_),
      peer_(other.peer_),
      transport_message_(other.transport_message_),
      send_timeout_(other.send_timeout_),
      receive_timeout_(other.receive_timeout_) {}

UdtConnection& UdtConnection::operator=(const UdtConnection &other) {
  udt_transport_ = other.udt_transport_;
  signals_ = other.signals_;
  threadpool_ = other.threadpool_;
  worker_ = other.worker_;
  udt_socket_id_ = other.udt_socket_id_;
  remote_ip_ = other.remote_ip_;
  remote_port_ = other.remote_port_;
  rendezvous_ip_ = other.rendezvous_ip_;
  rendezvous_port_ = other.rendezvous_port_;
  peer_ = other.peer_;
  transport_message_ = other.transport_message_;
  send_timeout_ = other.send_timeout_;
  receive_timeout_ = other.receive_timeout_;
  return *this;
}

UdtConnection::~UdtConnection() {
  if (worker_.get() != NULL)
    worker_->join();
}

void UdtConnection::Init() {
  if (!ValidIP(remote_ip_) || !ValidPort(remote_port_)) {
    DLOG(ERROR) << "Incorrect remote endpoint. " << remote_ip_ << ":" <<
        remote_port_ << std::endl;
    udt_socket_id_ = UDT::INVALID_SOCK;
    return;
  }
  if (!rendezvous_ip_.empty() &&
      (!ValidIP(rendezvous_ip_) || !ValidPort(rendezvous_port_))) {
    DLOG(ERROR) << "Incorrect rendezvous endpoint. " << rendezvous_ip_ << ":" <<
        rendezvous_port_ << std::endl;
    udt_socket_id_ = UDT::INVALID_SOCK;
    return;
  }
  if (udtutils::GetNewSocket(remote_ip_, remote_port_, false,
                             &udt_socket_id_, &peer_) != kSuccess)
    udt_socket_id_ = UDT::INVALID_SOCK;
}

void UdtConnection::Send(const TransportMessage &transport_message,
                         const int &response_timeout) {
  transport_message_ = transport_message;
  send_timeout_ = response_timeout < 0 ? kDefaultSendTimeout : response_timeout;
  receive_timeout_ = response_timeout < 0 ? 0 : response_timeout;
  boost::function<void()> functor =
      boost::bind(&UdtConnection::ConnectThenSend, *this);
  if (threadpool_.get()) {
    threadpool_->EnqueueTask(functor);
  } else {
    worker_.reset(new boost::thread(functor));
  }
}

void UdtConnection::SendResponse(const TransportMessage &transport_message) {
  transport_message_ = transport_message;
  send_timeout_ = kDefaultSendTimeout;
  receive_timeout_ = 0;
  boost::function<void()> functor =
      boost::bind(&UdtConnection::SendData, *this);
  if (threadpool_.get()) {
    threadpool_->EnqueueTask(functor);
  } else {
    worker_.reset(new boost::thread(functor));
  }
}

void UdtConnection::ConnectThenSend() {
  TransportCondition transport_condition =
      udtutils::Connect(udt_socket_id_, peer_);
  if (transport_condition != kSuccess) {
    signals_->on_send_(udt_socket_id_, kSendUdtFailure);
    return;
  }
  SendData();
}

void UdtConnection::SendData() {
  // Set timeout
  if (send_timeout_ > 0) {
    UDT::setsockopt(udt_socket_id_, 0, UDT_SNDTIMEO, &send_timeout_,
                    sizeof(send_timeout_));
  }

  // Send the message size
  TransportCondition result = SendDataSize();
  if (result != kSuccess) {
    signals_->on_send_(udt_socket_id_, result);
    UDT::close(udt_socket_id_);
    return;
  }

  // Send the message
  boost::shared_ptr<UdtStats> udt_stats(new UdtStats(udt_socket_id_,
                                                     UdtStats::kSend));
  result = SendDataContent();
  signals_->on_send_(udt_socket_id_, result);
  if (result != kSuccess) {
    UDT::close(udt_socket_id_);
    return;
  }

  // Get stats
  if (UDT::ERROR == UDT::perfmon(udt_socket_id_,
                                 &udt_stats->performance_monitor_)) {
#ifdef DEBUG
    if (UDT::getlasterror().getErrorCode() != 2001)
      DLOG(ERROR) << "UDT perfmon error: " <<
          UDT::getlasterror().getErrorMessage() << std::endl;
#endif
  } else {
    signals_->on_stats_(udt_stats);
  }
  if (receive_timeout_ > 0) {
    ReceiveData();
  } else if (receive_timeout_ == 0) {
    UDT::close(udt_socket_id_);
  }
}

TransportCondition UdtConnection::SendDataSize() {
  DataSize data_size = static_cast<DataSize>(transport_message_.ByteSize());
  if (data_size > kMaxTransportMessageSize) {
    DLOG(ERROR) << "Data size " << data_size << " bytes (exceeds limit of " <<
        kMaxTransportMessageSize << ")" << std::endl;
    return kMessageSizeTooLarge;
  }
  DataSize data_buffer_size = sizeof(DataSize);
  return MoveData(true, data_buffer_size, reinterpret_cast<char*>(&data_size));
}

TransportCondition UdtConnection::SendDataContent() {
  DataSize data_size = static_cast<DataSize>(transport_message_.ByteSize());
  boost::scoped_array<char> serialised_message(new char[data_size]);
  // Check for valid message
  if (!transport_message_.IsInitialized()) {
    DLOG(ERROR) << "UdtTransport::SendDataContent: uninitialised message." <<
        std::endl;
    return kInvalidData;
  }
  if (!transport_message_.SerializeToArray(serialised_message.get(),
                                           data_size)) {
    DLOG(ERROR) << "UdtTransport::SendDataContent: failed to serialise." <<
        std::endl;
    return kInvalidData;
  }
  return MoveData(true, data_size, serialised_message.get());
}

void UdtConnection::ReceiveData() {
  // Set timeout
  if (receive_timeout_ > 0) {
    UDT::setsockopt(udt_socket_id_, 0, UDT_RCVTIMEO, &receive_timeout_,
                    sizeof(receive_timeout_));
  }

  // Get the incoming message size
  DataSize data_size = ReceiveDataSize();
  if (data_size == 0)
    return;

  // Get message
  boost::shared_ptr<UdtStats> udt_stats(new UdtStats(udt_socket_id_,
                                                     UdtStats::kReceive));
  if (!ReceiveDataContent(data_size))
    return;

  // Get stats
  float rtt;
  if (UDT::ERROR == UDT::perfmon(udt_socket_id_,
                                 &udt_stats->performance_monitor_)) {
#ifdef DEBUG
    if (UDT::getlasterror().getErrorCode() != 2001)
      DLOG(ERROR) << "UDT perfmon error: " <<
          UDT::getlasterror().getErrorCode() << std::endl;
#endif
  } else {
    signals_->on_stats_(udt_stats);
    rtt = udt_stats->performance_monitor_.msRTT;
  }

  // Handle message
  HandleTransportMessage(rtt);
}

DataSize UdtConnection::ReceiveDataSize() {
  DataSize data_buffer_size = sizeof(DataSize);
  DataSize data_size;
  TransportCondition result =
      MoveData(false, data_buffer_size, reinterpret_cast<char*>(&data_size));
  if (result != kSuccess) {
    DLOG(ERROR) << "Cannot get data size." << std::endl;
    signals_->on_receive_(udt_socket_id_, result);
    UDT::close(udt_socket_id_);
    return 0;
  }
  if (data_size < 1) {
    LOG(ERROR) << "Data size is " << data_size << std::endl;
    signals_->on_receive_(udt_socket_id_, kReceiveSizeFailure);
    UDT::close(udt_socket_id_);
    return 0;
  }
  if (data_size > kMaxTransportMessageSize) {
    LOG(ERROR) << "Data size " << data_size << " bytes (exceeds limit of " <<
        kMaxTransportMessageSize << ")" << std::endl;
    signals_->on_receive_(udt_socket_id_, kMessageSizeTooLarge);
    UDT::close(udt_socket_id_);
    return 0;
  }
  return data_size;
}

bool UdtConnection::ReceiveDataContent(const DataSize &data_size) {
  transport_message_.Clear();
  boost::scoped_array<char> serialised_message(new char[data_size]);
  TransportCondition result =
      MoveData(false, data_size, serialised_message.get());
  if (result != kSuccess) {
    DLOG(ERROR) << "Cannot get data content." << std::endl;
    signals_->on_receive_(udt_socket_id_, result);
    UDT::close(udt_socket_id_);
    return false;
  }
  if (!transport_message_.ParseFromArray(serialised_message.get(), data_size)) {
    DLOG(ERROR) << "UdtTransport::ReceiveDataContent: failed to parse." <<
        std::endl;
    signals_->on_receive_(udt_socket_id_, kReceiveUdtFailure);
    UDT::close(udt_socket_id_);
    return false;
  }
  return true;
}

TransportCondition UdtConnection::MoveData(bool sending,
                                           const DataSize &data_size,
                                           char *data) {
  DataSize moved_total = 0;
  int moved_size = 0;
  boost::posix_time::time_duration timeout =
      (sending ? boost::posix_time::milliseconds(send_timeout_) :
                 boost::posix_time::milliseconds(receive_timeout_));
  boost::posix_time::ptime last_success =
      boost::posix_time::microsec_clock::universal_time();
  while (moved_total < data_size) {
    if (sending) {
      moved_size = UDT::send(udt_socket_id_, data + moved_total,
                             data_size - moved_total, 0);
    } else {
      moved_size = UDT::recv(udt_socket_id_, data + moved_total,
                             data_size - moved_total, 0);
    }
    if (UDT::ERROR == moved_size) {
      LOG(ERROR) << (sending ? "Send: " : "Recv: ") <<
          UDT::getlasterror().getErrorMessage() << std::endl;
      return (sending ? kSendUdtFailure : kReceiveUdtFailure);
    } else if (moved_size == 0) {
      if ((boost::posix_time::microsec_clock::universal_time() - last_success) >
          timeout) {
        LOG(INFO) << (sending ? "Sending socket " : "Receiving socket ") <<
            udt_socket_id_ << " timed out in MoveData." << std::endl;
        return (sending ? kSendTimeout : kReceiveTimeout);
      } else {
        boost::this_thread::sleep(boost::posix_time::milliseconds(1));
        continue;
      }
    }
    last_success = boost::posix_time::microsec_clock::universal_time();
    moved_total += moved_size;
  }
  return kSuccess;
}

bool UdtConnection::HandleTransportMessage(const float &rtt) {
  bool is_request(transport_message_.type() == TransportMessage::kRequest);
  // message data should contain exactly one optional field
  const google::protobuf::Message::Reflection *reflection =
      transport_message_.data().GetReflection();
  std::vector<const google::protobuf::FieldDescriptor*> field_descriptors;
  reflection->ListFields(transport_message_.data(), &field_descriptors);
  if (field_descriptors.size() != 1U) {
    LOG(INFO) << "Bad data - doesn't contain exactly one field." << std::endl;
    if (!is_request)
      signals_->on_receive_(udt_socket_id_, kReceiveParseFailure);
    UDT::close(udt_socket_id_);
    return false;
  }
  switch (field_descriptors.at(0)->number()) {
    case TransportMessage::Data::kRawMessageFieldNumber:
      signals_->on_message_received_(transport_message_.data().raw_message(),
                                     udt_socket_id_, rtt);
      break;
    case TransportMessage::Data::kRpcMessageFieldNumber:
      if (is_request) {
        signals_->on_rpc_request_received_(
            transport_message_.data().rpc_message(), udt_socket_id_, rtt);
        // Leave socket open to send response on.
      } else {
        signals_->on_rpc_response_received_(
            transport_message_.data().rpc_message(), udt_socket_id_, rtt);
        UDT::close(udt_socket_id_);
      }
      break;
    case TransportMessage::Data::kHolePunchingMessageFieldNumber:
//      HandleRendezvousMessage(
//          transport_message_.data().hole_punching_message());
      UDT::close(udt_socket_id_);
      break;
    case TransportMessage::Data::kPingFieldNumber:
      UDT::close(udt_socket_id_);
      break;
    case TransportMessage::Data::kProxyPingFieldNumber:
      UDT::close(udt_socket_id_);
      break;
    case TransportMessage::Data::kManagedEndpointMessageFieldNumber:
      if (udt_transport_) {
        if (is_request) {
          udt_transport_->HandleManagedSocketRequest(udt_socket_id_,
              transport_message_.data().managed_endpoint_message());
          // Leave socket open.
        } else {
          udt_transport_->HandleManagedSocketResponse(udt_socket_id_,
              transport_message_.data().managed_endpoint_message());
          // Leave socket open.
        }
      } else {
        UDT::close(udt_socket_id_);
      }
      break;
    default:
      LOG(INFO) << "Unrecognised data type in TransportMessage." << std::endl;
      UDT::close(udt_socket_id_);
      return false;
  }
  return true;
}

}  // namespace transport