/* Copyright (c) 2010 maidsafe.net limited
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

#include "maidsafe-dht/transport/udt_control_packet.h"

#include <cassert>
#include <cstring>

namespace asio = boost::asio;

namespace maidsafe {

namespace transport {

UdtControlPacket::UdtControlPacket()
  : type_(0),
    additional_info_(0),
    time_stamp_(0),
    destination_socket_id_(0) {
}

UdtControlPacket::~UdtControlPacket() {
}

boost::uint16_t UdtControlPacket::Type() const {
  return type_;
}

void UdtControlPacket::SetType(boost::uint16_t n) {
  assert(n <= 0x7fff);
  type_ = n;
}

boost::uint32_t UdtControlPacket::AdditionalInfo() const {
  return additional_info_;
}

void UdtControlPacket::SetAdditionalInfo(boost::uint32_t n) {
  assert(n <= 0x1fffffff);
  additional_info_ = n;
}

boost::uint32_t UdtControlPacket::TimeStamp() const {
  return time_stamp_;
}

void UdtControlPacket::SetTimeStamp(boost::uint32_t n) {
  time_stamp_ = n;
}

boost::uint32_t UdtControlPacket::DestinationSocketId() const {
  return destination_socket_id_;
}

void UdtControlPacket::SetDestinationSocketId(boost::uint32_t n) {
  destination_socket_id_ = n;
}

bool UdtControlPacket::IsValidBase(const asio::const_buffer &buffer) {
  return ((asio::buffer_size(buffer) >= 16) &&
          ((asio::buffer_cast<const unsigned char *>(buffer)[0] & 0x80) != 0));
}

bool UdtControlPacket::DecodeBase(const asio::const_buffer &buffer) {
  // Refuse to decode if the input buffer is not valid.
  if (!IsValidBase(buffer))
    return false;

  const unsigned char *p = asio::buffer_cast<const unsigned char *>(buffer);
  size_t length = asio::buffer_size(buffer);

  type_ = (p[0] & 0x7f);
  type_ = ((type_ << 8) | p[1]);
  additional_info_ = (p[4] & 0x1f);
  additional_info_ = ((additional_info_ << 8) | p[5]);
  additional_info_ = ((additional_info_ << 8) | p[6]);
  additional_info_ = ((additional_info_ << 8) | p[7]);
  DecodeUint32(&time_stamp_, p + 8);
  DecodeUint32(&destination_socket_id_, p + 12);

  return true;
}

size_t UdtControlPacket::EncodeBase(const asio::mutable_buffer &buffer) const {
  // Refuse to encode if the output buffer is not big enough.
  if (asio::buffer_size(buffer) < kHeaderSize)
    return 0;

  unsigned char *p = asio::buffer_cast<unsigned char *>(buffer);

  p[0] = ((type_ >> 8) & 0x7f);
  p[0] |= 0x80;
  p[1] = (type_ & 0xff);
  p[2] = p[3] = 0;
  p[4] = ((additional_info_ >> 24) & 0x1f);
  p[5] = ((additional_info_ >> 16) & 0xff);
  p[6] = ((additional_info_ >> 8) & 0xff);
  p[7] = (additional_info_  & 0xff);
  EncodeUint32(time_stamp_, p + 8);
  EncodeUint32(destination_socket_id_, p + 12);

  return kHeaderSize;
}

}  // namespace transport

}  // namespace maidsafe