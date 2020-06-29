/*
 * nghttp2 - HTTP/2 C Library
 *
 * Copyright (c) 2014 Tatsuhiro Tsujikawa
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
// We wrote this code based on the original code which has the
// following license:
//
// connection.hpp
// ~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2013 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_SERVER_CONNECTION_H
#define ASIO_SERVER_CONNECTION_H

#include "nghttp2_config.h"

#include <memory>

#include <boost/noncopyable.hpp>
#include <boost/array.hpp>

#include <nghttp2/asio_http2_server.h>

#include "asio_server_http2_handler.h"
#include "asio_server_serve_mux.h"
#include "util.h"
#include "template.h"

#include <iostream>

#if BOOST_VERSION >= 107000
#  define GET_IO_SERVICE(s)                                                    \
    ((boost::asio::io_context &)(s).get_executor().context())
#else
#  define GET_IO_SERVICE(s) ((s).get_io_service())
#endif

namespace nghttp2 {

namespace asio_http2 {

namespace server {

/// Represents a single connection from a client.
template <typename socket_type>
class connection : public std::enable_shared_from_this<connection<socket_type>>,
                   private boost::noncopyable {
public:
  /// Construct a connection with the given io_service.
  template <typename... SocketArgs>
  explicit connection(
      serve_mux &mux,
      std::shared_ptr<bool> &go_away,
      const boost::posix_time::time_duration &tls_handshake_timeout,
      const boost::posix_time::time_duration &read_timeout,
      uint32_t max_concurrent_streams,
      SocketArgs &&... args)
      : socket_(std::forward<SocketArgs>(args)...),
        mux_(mux),
        go_away_(go_away),
        deadline_(GET_IO_SERVICE(socket_)),
        tls_handshake_timeout_(tls_handshake_timeout),
        read_timeout_(read_timeout),
        max_concurrent_streams_(max_concurrent_streams),
        writing_(false),
        stopped_(false) {}

  /// Start the first asynchronous operation for the connection.
  void start() {
    boost::system::error_code ec;
    std::weak_ptr<connection> weak_self = this->shared_from_this();

    handler_ = std::make_shared<http2_handler>(
        GET_IO_SERVICE(socket_), socket_.lowest_layer().remote_endpoint(ec),
        [weak_self]() {
          auto shared_connection = weak_self.lock();
          if (!shared_connection) { return; }
          shared_connection->do_write();
        }, mux_, max_concurrent_streams_);
    if (handler_->start() != 0) {
      stop();
      return;
    }
    do_read();
  }

  socket_type &socket() { return socket_; }

  void start_tls_handshake_deadline() {
    deadline_.expires_from_now(tls_handshake_timeout_);
    deadline_.async_wait(
        std::bind(&connection::handle_deadline, this->shared_from_this()));
  }

  void start_read_deadline() {
    deadline_.expires_from_now(read_timeout_);
    deadline_.async_wait(
        std::bind(&connection::handle_deadline, this->shared_from_this()));
  }

  void handle_deadline() {
    if (stopped_) {
      return;
    }

    if (*go_away_ && !handler_->should_stop()) {
      handler_->terminate();
      do_write();
      return;
    }

    if (deadline_.expires_at() <=
        boost::asio::deadline_timer::traits_type::now()) {
      stop();
      deadline_.expires_at(boost::posix_time::pos_infin);
      return;
    }

    deadline_.async_wait(
        std::bind(&connection::handle_deadline, this->shared_from_this()));
  }

  void do_read() {
    auto self = this->shared_from_this();

    deadline_.expires_from_now(read_timeout_);

    socket_.async_read_some(
        boost::asio::buffer(buffer_),
        [this, self](const boost::system::error_code &e,
                     std::size_t bytes_transferred) {
          if (e) {
            stop();
            return;
          }

          if (handler_->on_read(buffer_, bytes_transferred) != 0) {
            stop();
            return;
          }

          if (*go_away_ && !handler_->should_stop()) {
            handler_->terminate();
          }

          do_write();

          if (!writing_ && handler_->should_stop()) {
            stop();
            return;
          }

          do_read();

          // If an error occurs then no new asynchronous operations are
          // started. This means that all shared_ptr references to the
          // connection object will disappear and the object will be
          // destroyed automatically after this handler returns. The
          // connection class's destructor closes the socket.
        });
  }

  void do_write() {
    auto self = this->shared_from_this();

    if (writing_) {
      return;
    }

    int rv;
    std::size_t nwrite;

    rv = handler_->on_write(outbuf_, nwrite);

    if (rv != 0) {
      stop();
      return;
    }

    if (nwrite == 0) {
      if (handler_->should_stop()) {
        stop();
      }
      return;
    }

    writing_ = true;

    // Reset read deadline here, because normally client is sending
    // something, it does not expect timeout while doing it.
    deadline_.expires_from_now(read_timeout_);

    boost::asio::async_write(
        socket_, boost::asio::buffer(outbuf_, nwrite),
        [this, self](const boost::system::error_code &e, std::size_t) {
          if (e) {
            stop();
            return;
          }

          writing_ = false;

          do_write();
        });

    // No new asynchronous operations are started. This means that all
    // shared_ptr references to the connection object will disappear and
    // the object will be destroyed automatically after this handler
    // returns. The connection class's destructor closes the socket.
  }

  void stop() {
    if (stopped_) {
      return;
    }

    stopped_ = true;
    boost::system::error_code ignored_ec;
    socket_.lowest_layer().close(ignored_ec);
    deadline_.cancel();
  }

private:
  socket_type socket_;

  serve_mux &mux_;

  std::shared_ptr<bool> go_away_;

  std::shared_ptr<http2_handler> handler_;

  /// Buffer for incoming data.
  boost::array<uint8_t, 8_k> buffer_;

  boost::array<uint8_t, 64_k> outbuf_;

  boost::asio::deadline_timer deadline_;
  boost::posix_time::time_duration tls_handshake_timeout_;
  boost::posix_time::time_duration read_timeout_;
  uint32_t max_concurrent_streams_;

  bool writing_;
  bool stopped_;
};

} // namespace server

} // namespace asio_http2

} // namespace nghttp2

#endif // ASIO_SERVER_CONNECTION_H
