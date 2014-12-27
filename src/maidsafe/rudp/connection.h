/*  Copyright 2012 MaidSafe.net limited

    This MaidSafe Software is licensed to you under (1) the MaidSafe.net Commercial License,
    version 1.0 or later, or (2) The General Public License (GPL), version 3, depending on which
    licence you accepted on initial access to the Software (the "Licences").

    By contributing code to the MaidSafe Software, or to this project generally, you agree to be
    bound by the terms of the MaidSafe Contributor Agreement, version 1.0, found in the root
    directory of this project at LICENSE, COPYING and CONTRIBUTOR respectively and also
    available at: http://www.maidsafe.net/licenses

    Unless required by applicable law or agreed to in writing, the MaidSafe Software distributed
    under the GPL Licence is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS
    OF ANY KIND, either express or implied.

    See the Licences for the specific language governing permissions and limitations relating to
    use of the MaidSafe Software.                                                                 */

// Original author: Christopher M. Kohlhoff (chris at kohlhoff dot com)

#ifndef MAIDSAFE_RUDP_CONNECTION_H_
#define MAIDSAFE_RUDP_CONNECTION_H_

#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#include "boost/asio/deadline_timer.hpp"
#include "boost/asio/io_service.hpp"
#include "boost/asio/ip/udp.hpp"
#include "boost/asio/strand.hpp"

#include "maidsafe/rudp/core/socket.h"
#include "maidsafe/rudp/transport.h"
#include "maidsafe/rudp/types.h"

namespace maidsafe {

namespace rudp {

namespace detail {

typedef int32_t DataSize;

class Multiplexer;

class Connection : public std::enable_shared_from_this<Connection> {
 public:
  enum class State {
    // GetAvailableEndpoint has been called, but connection has not yet been made.
    kPending,
    // Not used for sending messages; ping, peer external IP or NAT detection, etc.
    kTemporary,
    // Incoming or outgoing short-lived (unvalidated) connection - classed as a "normal" connection.
    kBootstrapping,
    // Permanent connection which has not been validated - classed as a "normal" connection.
    kUnvalidated,
    // Validated permanent connection - classed as a "normal" connection.
    kPermanent,
    // A duplicate "normal" connection already exists on a different Transport.  Will be closed
    // without triggering callback as soon as state set to this.
    kDuplicate,
  };

  using ConnectionPtr = std::shared_ptr<Connection>;
  using ExtErrorCode = std::error_code;
  using ErrorCode = boost::system::error_code;
  using OnConnect = std::function<void(const ExtErrorCode&, const ConnectionPtr&)>;

  Connection(const std::shared_ptr<Transport>& transport,
             const boost::asio::io_service::strand& strand,
             std::shared_ptr<Multiplexer> multiplexer);

  detail::Socket& Socket();

  void Close();
  // If lifespan is 0, only handshaking will be done.  Otherwise, the connection will be closed
  // after lifespan has passed.
  void StartConnecting(const NodeId& peer_node_id, const asio::ip::udp::endpoint& peer_endpoint,
                       const asymm::PublicKey& peer_public_key,
                       ConnectionAddedFunctor connection_added_functor,
                       const boost::posix_time::time_duration& connect_attempt_timeout,
                       const boost::posix_time::time_duration& lifespan, OnConnect on_connect,
                       const std::function<void()>& failure_functor);
  void Ping(const NodeId& peer_node_id, const asio::ip::udp::endpoint& peer_endpoint,
            const asymm::PublicKey& peer_public_key,
            const std::function<void(int)>& ping_functor);  // NOLINT (Fraser)
  void StartSending(const std::string& data, const MessageSentFunctor& message_sent_functor);
  State state() const;
  // Sets the state_ to kPermanent or kUnvalidated and sets the lifespan_timer_ to expire at
  // pos_infin.
  void MakePermanent(bool validated);
  void MarkAsDuplicateAndClose();
  std::function<void()> GetAndClearFailureFunctor();

  // Get the remote endpoint offered for NAT detection.
  asio::ip::udp::endpoint RemoteNatDetectionEndpoint() const;
  // Helpers for debugging
  boost::posix_time::time_duration ExpiresFromNow() const;
  std::string PeerDebugId() const;

  asio::ip::udp::endpoint PeerEndpoint() const { return peer_endpoint_; }
  NodeId PeerNodeId() const { return peer_node_id_; }

 private:
  Connection(const Connection&);
  Connection& operator=(const Connection&);

  struct SendRequest {
    std::string encrypted_data_;
    MessageSentFunctor handler_;

    SendRequest(std::string encrypted_data,
                std::function<void(error_code)> message_sent_functor)  // NOLINT (Dan)
#if defined(__GLIBCXX__)
        //  && __GLIBCXX__ < date (date in format of 20141218 as the date of fix of COW string)
        : encrypted_data_(encrypted_data.data(), encrypted_data.size()),
#else
        : encrypted_data_(std::move(encrypted_data)),
#endif
          handler_(std::move(message_sent_functor)) {
    }
  };

  void DoClose(const ExtErrorCode&, int debug_line_no);

  void DoStartConnecting(const NodeId& peer_node_id, const asio::ip::udp::endpoint& peer_endpoint,
                         const asymm::PublicKey& peer_public_key,
                         ConnectionAddedFunctor connection_added_functor,
                         const boost::posix_time::time_duration& connect_attempt_timeout,
                         const boost::posix_time::time_duration& lifespan,
                         const std::function<void(int)>& ping_functor,  // NOLINT (Fraser)
                         const OnConnect& on_connect, const std::function<void()>& failure_functor);
  void DoStartSending(SendRequest request);  // NOLINT (Fraser)
  void DoQueueSendRequest(SendRequest request);
  void FinishSendAndQueueNext();

  void CheckTimeout(const ErrorCode& ec);
  void CheckLifespanTimeout(const ErrorCode& ec);
  bool Stopped() const;
  bool TicksStopped() const;

  void StartTick();
  void HandleTick();

  void StartConnect(const asymm::PublicKey& peer_public_key,
                    const boost::posix_time::time_duration& connect_attempt_timeout,
                    const boost::posix_time::time_duration& lifespan,
                    const std::function<void(int)>& ping_functor);  // NOLINT (Fraser)
  void HandleConnect(const ErrorCode& ec,
                     std::function<void(int)> ping_functor);  // NOLINT (Fraser)

  void StartReadSize();
  void HandleReadSize(const ErrorCode& ec);

  void StartReadData();
  void HandleReadData(const ErrorCode& ec, size_t length);

  void StartWrite(const MessageSentFunctor& message_sent_functor);  // NOLINT (Fraser)
  void HandleWrite(MessageSentFunctor message_sent_functor);        // NOLINT (Fraser)

  void StartProbing();
  void DoProbe(const ErrorCode& ec);
  void HandleProbe(const ErrorCode& ec);

  void DoMakePermanent(bool validated);

  void EncodeData(const std::string& data);

  void InvokeSentFunctor(const MessageSentFunctor& message_sent_functor,
                         const ExtErrorCode& error) const;

  void FireOnConnectFunctor(const ExtErrorCode&);
  void FireConnectionAddedFunctor(const ExtErrorCode&);

  std::weak_ptr<Transport> transport_;
  boost::asio::io_service::strand strand_;
  std::shared_ptr<Multiplexer> multiplexer_;
  detail::Socket socket_;
  uint32_t cookie_syn_;
  boost::asio::deadline_timer timer_, probe_interval_timer_, lifespan_timer_;
  NodeId peer_node_id_;
  asio::ip::udp::endpoint peer_endpoint_;
  ConnectionAddedFunctor connection_added_functor_;
  std::vector<unsigned char> send_buffer_, receive_buffer_;
  DataSize data_size_, data_received_;
  uint8_t failed_probe_count_;
  State state_;
  mutable std::mutex state_mutex_;
  enum class TimeoutState { kConnecting, kConnected, kClosing } timeout_state_;
  bool sending_;
  std::function<void()> failure_functor_;
  std::queue<SendRequest> send_queue_;
  std::mutex handle_tick_lock_;

  OnConnect on_connect_;
};

template <typename Elem, typename Traits>
std::basic_ostream<Elem, Traits>& operator<<(std::basic_ostream<Elem, Traits>& ostream,
                                             const Connection::State& state) {
  std::string state_str;
  switch (state) {
    case Connection::State::kPending:
      state_str = "Pending";
      break;
    case Connection::State::kTemporary:
      state_str = "Temporary";
      break;
    case Connection::State::kBootstrapping:
      state_str = "Bootstrapping";
      break;
    case Connection::State::kUnvalidated:
      state_str = "Unvalidated";
      break;
    case Connection::State::kPermanent:
      state_str = "Permanent";
      break;
    default:
      state_str = "Invalid";
      break;
  }

  for (auto& ch : state_str)
    ostream << ostream.widen(ch);
  return ostream;
}

}  // namespace detail

}  // namespace rudp

}  // namespace maidsafe

#endif  // MAIDSAFE_RUDP_CONNECTION_H_
