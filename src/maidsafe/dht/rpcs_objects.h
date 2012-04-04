/* Copyright (c) 2011 maidsafe.net limited
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

#ifndef MAIDSAFE_DHT_RPCS_OBJECTS_H_
#define MAIDSAFE_DHT_RPCS_OBJECTS_H_

#include <map>
#include <string>
#include <utility>

#include "boost/thread/mutex.hpp"
#include "boost/thread/locks.hpp"

#include "maidsafe/dht/config.h"
#include "maidsafe/dht/version.h"

#if MAIDSAFE_DHT_VERSION != 3300
#  error This API is not compatible with the installed library.\
    Please update the maidsafe-dht library.
#endif


namespace maidsafe  {

namespace dht {

// This class temporarily holds the connected objects of Rpcs to ensure all
// resources can be correctly released and no memory leaked
class ConnectedObjectsList  {
 public:
  ConnectedObjectsList();

  ~ConnectedObjectsList();
  // Adds a connected object into the container
  // return the index of those objects in the container
  uint32_t AddObject(const TransportPtr transport,
                     const MessageHandlerPtr message_handler);

  // Remove an object based on the index
  // Returns true if successfully removed or false otherwise.
  bool RemoveObject(uint32_t index);

  // Return the TransportPtr of the index
  TransportPtr GetTransport(uint32_t index);

  // Returns the size of the connected objects container
  size_t Size();

 private:
  std::map<uint32_t, std::pair<TransportPtr, MessageHandlerPtr>>
      objects_container_;
  boost::mutex mutex_;
  uint32_t index_;
};

}  // namespace dht

}  // namespace maidsafe

#endif  // MAIDSAFE_DHT_RPCS_OBJECTS_H_
