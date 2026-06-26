#include <trx_provider.hpp>
#include <http_client_async.hpp>

#include <sysio/net_plugin/net_utils.hpp>
#include <sysio/net_plugin/protocol.hpp>
#include <sysio/chain/exceptions.hpp>

#include <fc/io/raw.hpp>
#include <fc/io/json.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unistd.h>

namespace sysio::testing {
   using namespace boost::asio;
   using namespace std::literals::string_literals;
   using boost::asio::ip::tcp;

   constexpr auto message_header_size = sizeof(uint32_t);
   constexpr uint32_t trx_msg_which = sysio::to_index(sysio::msg_type_t::transaction_message);
   static_assert(trx_msg_which == fc::get_index<sysio::net_message, sysio::transaction_message>());

   constexpr uint16_t trx_generator_advertised_port = 0;
   constexpr std::string_view trx_generator_advertised_host = "127.0.0.1";
   constexpr std::string_view trx_generator_node_id_domain = "trx_generator_p2p_node";
   constexpr uint32_t bytes_per_kibibyte = 1024;
   /// Maximum inbound peer control frame size; trx-only peers never receive block payloads from nodeop.
   constexpr uint32_t max_trx_generator_peer_message_bytes = bytes_per_kibibyte * bytes_per_kibibyte;
   constexpr int provider_disconnect_wait_seconds = 30;

   std::atomic<uint64_t> trx_generator_connection_nonce{0};

   /** Serializes a net_message payload with the fixed-size P2P message length prefix. */
   template<typename Message>
   static send_buffer_type create_net_message_send_buffer(const Message& m) {
      const net_message net_msg{m};
      const uint32_t payload_size = fc::raw::pack_size(net_msg);
      const size_t buffer_size = message_header_size + payload_size;
      // Avoid variable-size encoding for the fixed-width P2P length prefix.
      const char* const header = reinterpret_cast<const char* const>(&payload_size);

      auto send_buffer = std::make_shared<std::vector<char>>(buffer_size);
      fc::datastream<char*> ds(send_buffer->data(), buffer_size);
      ds.write(header, message_header_size);
      fc::raw::pack(ds, net_msg);

      return send_buffer;
   }

   /** Returns a synthetic peer identity for the transaction generator's short-lived P2P session. */
   static fc::sha256 make_trx_generator_node_id(const provider_base_config& config) {
      std::ostringstream ss;
      ss << trx_generator_node_id_domain << ':' << getpid() << ':' << trx_generator_connection_nonce.fetch_add(1)
         << ':' << config._peer_endpoint << ':' << config._port << ':'
         << fc::time_point::now().time_since_epoch().count();
      return fc::sha256::hash(ss.str());
   }

   /** Builds the minimal P2P handshake needed before streaming transaction_message frames. */
   static handshake_message make_trx_generator_handshake(const provider_base_config& config,
                                                         const chain::chain_id_type& chain_id,
                                                         const fc::sha256& node_id,
                                                         const chain::block_id_type& fork_db_root_id,
                                                         const chain::block_id_type& fork_db_head_id,
                                                         const chain::block_id_type& chain_head_id,
                                                         int16_t generation) {
      handshake_message handshake;
      handshake.network_version = wire_protocol_base_version;
      handshake.chain_id = chain_id;
      handshake.node_id = node_id;
      handshake.fork_db_root_id = fork_db_root_id;
      handshake.fork_db_head_id = fork_db_head_id;
      handshake.chain_head_id = chain_head_id;
      handshake.generation = generation;
      handshake.p2p_address = std::string(trx_generator_advertised_host) + ":" +
                              std::to_string(trx_generator_advertised_port) + ":" +
                              std::string(net_utils::trx_connection_type);
      handshake.agent = std::string(trx_generator_agent);
      return handshake;
   }

   /** Returns true when no concrete block has been learned for a chain reference yet. */
   static bool is_default_block_id(const chain::block_id_type& id) {
      return id == chain::block_id_type{};
   }

   /** Returns the current wall-clock timestamp in the P2P time_message unit. */
   static int64_t current_time_ns() {
      return std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch())
            .count();
   }

   static send_buffer_type create_send_buffer( const chain::packed_transaction& m ) {
      // Build wire bytes matching transaction_message: [size][which][trx_id][packed_transaction]
      const auto& id = m.id();
      const uint32_t which_size = fc::raw::pack_size(chain::unsigned_int(trx_msg_which));
      const uint32_t id_size = fc::raw::pack_size( id );
      const uint32_t payload_size = which_size + id_size + fc::raw::pack_size( m );
      const size_t buffer_size = message_header_size + payload_size;

      // Avoid variable-size encoding for the fixed-width P2P length prefix.
      const char* const header = reinterpret_cast<const char* const>(&payload_size);

      auto send_buffer = std::make_shared<std::vector<char>>(buffer_size);
      fc::datastream<char*> ds( send_buffer->data(), buffer_size);
      ds.write( header, message_header_size );
      fc::raw::pack( ds, fc::unsigned_int(trx_msg_which));
      fc::raw::pack( ds, id );
      fc::raw::pack( ds, m );

      return send_buffer;
   }

   void provider_connection::init_and_connect(const chain::chain_id_type& chain_id,
                                              const chain::block_id_type& last_irreversible_block_id) {
      _connection_thread_pool.start(1,
                                    [&](const fc::exception &e) {
                                       wlog("Exception in connection_thread: {}", e.to_detail_string());
                                    });
      connect(chain_id, last_irreversible_block_id);
   };

   void provider_connection::cleanup_and_disconnect() {
      disconnect();
      _connection_thread_pool.stop();
   };

   fc::time_point provider_connection::get_trx_ack_time(const sysio::chain::transaction_id_type& trx_id) {
      fc::time_point              time_acked;
      std::lock_guard<std::mutex> lock(_trx_ack_map_lock);
      auto                        search = _trxs_ack_time_map.find(trx_id);
      if (search != _trxs_ack_time_map.end()) {
         time_acked = search->second;
      } else {
         elog("get_trx_ack_time - Transaction acknowledge time not found for transaction with id: {}", trx_id);
         time_acked = fc::time_point::min();
      }
      return time_acked;
   }

   void provider_connection::trx_acknowledged(const sysio::chain::transaction_id_type& trx_id,
                                              const fc::time_point&                    ack_time) {
      std::lock_guard<std::mutex> lock(_trx_ack_map_lock);
      _trxs_ack_time_map[trx_id] = ack_time;
   }

   chain::block_id_type provider_connection::reference_block_id(const chain::block_id_type& fallback) const {
      return fallback;
   }

   void provider_connection::throw_if_unhealthy() const {}

   void p2p_connection::connect(const chain::chain_id_type& chain_id,
                                const chain::block_id_type& last_irreversible_block_id) {
      _node_id = make_trx_generator_node_id(_config);
      _chain_id = chain_id;
      _sent_handshake_generation = 0;
      _transport_failed.store(false, std::memory_order_release);
      {
         std::lock_guard<std::mutex> lock(_peer_chain_state_lock);
         _latest_fork_db_root_id = last_irreversible_block_id;
         _latest_fork_db_head_id = last_irreversible_block_id;
         _latest_chain_head_id = last_irreversible_block_id;
      }

      ilog("Attempting P2P connection to {}:{}.", _config._peer_endpoint, _config._port);
      tcp::resolver r(_connection_thread_pool.get_executor());
      auto i = r.resolve(tcp::v4(), _config._peer_endpoint, std::to_string(_config._port));
      boost::asio::connect(_p2p_socket, i);
      ilog("Connected to {}:{}.", _config._peer_endpoint, _config._port);
      if (!send_handshake()) {
         throw std::runtime_error("failed to send P2P transaction generator handshake");
      }
      ilog("Sent P2P transaction generator handshake to {}:{}.", _config._peer_endpoint, _config._port);
      read_peer_message_header();
   }

   void p2p_connection::close_socket() {
      boost::system::error_code ec;
      _p2p_socket.shutdown(tcp::socket::shutdown_both, ec);
      ec.clear();
      _p2p_socket.close(ec);
   }

   void p2p_connection::mark_transport_failed(std::string_view label, const boost::system::error_code& ec) {
      if (!_transport_failed.exchange(true, std::memory_order_acq_rel)) {
         ++_errors;
         elog("P2P {} write failed: {}: {}", std::string(label), ec.value(), ec.message());
         close_socket();
      }
   }

   void p2p_connection::mark_transport_failed(std::string_view label, std::string_view detail, bool count_error) {
      if (!_transport_failed.exchange(true, std::memory_order_acq_rel)) {
         if (count_error) {
            ++_errors;
            elog("P2P {} failed: {}", std::string(label), std::string(detail));
         } else {
            wlog("P2P {} closed: {}", std::string(label), std::string(detail));
         }
         close_socket();
      }
   }

   bool p2p_connection::write_message(const send_buffer_type& msg, std::string_view label) {
      if (_transport_failed.load(std::memory_order_acquire)) {
         return false;
      }

      boost::system::error_code ec;
      boost::asio::write(_p2p_socket, boost::asio::buffer(*msg), ec);
      if (ec) {
         mark_transport_failed(label, ec);
         return false;
      }
      return true;
   }

   bool p2p_connection::send_handshake() {
      if (_sent_handshake_generation == std::numeric_limits<int16_t>::max()) {
         _sent_handshake_generation = 1;
      }
      const int16_t generation = ++_sent_handshake_generation;

      chain::block_id_type fork_db_root_id;
      chain::block_id_type fork_db_head_id;
      chain::block_id_type chain_head_id;
      {
         std::lock_guard<std::mutex> lock(_peer_chain_state_lock);
         fork_db_root_id = _latest_fork_db_root_id;
         fork_db_head_id = _latest_fork_db_head_id;
         chain_head_id = _latest_chain_head_id;
      }

      auto handshake = create_net_message_send_buffer(make_trx_generator_handshake(_config, _chain_id, _node_id,
                                                                                   fork_db_root_id, fork_db_head_id,
                                                                                   chain_head_id, generation));
      return write_message(handshake, "handshake");
   }

   bool p2p_connection::handle_peer_handshake(const chain::block_id_type& fork_db_root_id,
                                              const chain::block_id_type& fork_db_head_id,
                                              const chain::block_id_type& chain_head_id) {
      {
         std::lock_guard<std::mutex> lock(_peer_chain_state_lock);
         _latest_fork_db_root_id = fork_db_root_id;
         _latest_fork_db_head_id = fork_db_head_id;
         _latest_chain_head_id = chain_head_id;
      }
      return send_handshake();
   }

   bool p2p_connection::send_time_response(int64_t origin_timestamp, int64_t receive_timestamp) {
      time_message response;
      response.org = origin_timestamp;
      response.rec = receive_timestamp;
      response.xmt = current_time_ns();
      response.dst = trx_generator_empty_time_message_timestamp;
      auto msg = create_net_message_send_buffer(response);
      return write_message(msg, "time response");
   }

   void p2p_connection::read_peer_message_header() {
      boost::asio::async_read(
            _p2p_socket, boost::asio::buffer(_read_header),
            boost::asio::bind_executor(_strand, [this](const boost::system::error_code& ec, std::size_t) {
         if (ec) {
            if (ec != boost::asio::error::operation_aborted && ec != boost::asio::error::eof) {
               wlog("P2P peer header read failed: {}: {}", ec.value(), ec.message());
            }
            return;
         }

         uint32_t payload_size = 0;
         std::memcpy(&payload_size, _read_header.data(), sizeof(payload_size));
         if (payload_size == 0 || payload_size > max_trx_generator_peer_message_bytes) {
            mark_transport_failed("peer message length", std::to_string(payload_size) + " is outside the supported range",
                                  true);
            return;
         }

         _read_payload.resize(payload_size);
         read_peer_message_payload(payload_size);
      }));
   }

   void p2p_connection::read_peer_message_payload(uint32_t payload_size) {
      boost::asio::async_read(
            _p2p_socket, boost::asio::buffer(_read_payload),
            boost::asio::bind_executor(
                  _strand, [this, payload_size](const boost::system::error_code& ec, std::size_t) {
         if (ec) {
            if (ec != boost::asio::error::operation_aborted && ec != boost::asio::error::eof) {
               wlog("P2P peer payload read failed: {}: {}", ec.value(), ec.message());
            }
            return;
         }

         const int64_t receive_time = current_time_ns();
         try {
            fc::datastream<const char*> ds(_read_payload.data(), payload_size);
            net_message msg;
            fc::raw::unpack(ds, msg);
            if (ds.remaining() != 0) {
               mark_transport_failed("peer message decode", "trailing bytes after net_message payload", true);
               return;
            }

            bool continue_reading = true;
            if (const auto* handshake = std::get_if<handshake_message>(&msg)) {
               continue_reading = handle_peer_handshake(handshake->fork_db_root_id, handshake->fork_db_head_id,
                                                        handshake->chain_head_id);
            } else if (const auto* peer_time = std::get_if<time_message>(&msg);
                       peer_time != nullptr && peer_time->org == trx_generator_empty_time_message_timestamp &&
                       peer_time->xmt != trx_generator_empty_time_message_timestamp) {
               continue_reading = send_time_response(peer_time->xmt, receive_time);
            } else if (const auto* go_away = std::get_if<go_away_message>(&msg)) {
               wlog("P2P peer sent go_away_message: {}", reason_str(go_away->reason));
               mark_transport_failed("peer go_away_message", reason_str(go_away->reason), false);
               continue_reading = false;
            }

            if (!continue_reading || _transport_failed.load(std::memory_order_acquire)) {
               return;
            }
         } catch (const fc::exception& e) {
            ++_errors;
            elog("Failed to unpack P2P peer message: {}", e.to_detail_string());
            return;
         } catch (const std::exception& e) {
            ++_errors;
            elog("Failed to unpack P2P peer message: {}", e.what());
            return;
         }

         read_peer_message_header();
      }));
   }

   void p2p_connection::disconnect() {
      int max    = provider_disconnect_wait_seconds;
      int waited = 0;
      for (uint64_t sent = _sent.load(), sent_callback_num = _sent_callback_num.load();
           sent != sent_callback_num && !_transport_failed.load(std::memory_order_acquire) && waited < max;
           sent = _sent.load(), sent_callback_num = _sent_callback_num.load()) {
         ilog("disconnect waiting on ack - sent {} | acked {} | waited {}",
              sent, sent_callback_num, waited);
         sleep(1);
         ++waited;
      }
      if (_sent.load() != _sent_callback_num.load()) {
         elog("disconnect stopped before receiving all acks - sent {} | acked {} | waited {}",
              _sent.load(), _sent_callback_num.load(), waited);
      }
      close_socket();
      if (_errors.load()) {
         elog("{} errors reported during p2p calls, see logs", _errors.load());
      }
   }

   void p2p_connection::send_transaction(const chain::packed_transaction& trx) {
      throw_if_unhealthy();
      send_buffer_type msg = create_send_buffer(trx);

      ++_sent;
      boost::asio::post( _strand, [this, msg{std::move(msg)}, id{trx.id()}]() {
         if (write_message(msg, "transaction")) {
            trx_acknowledged(id, fc::time_point::min()); //using min to identify ack time as not applicable for p2p
            ++_sent_callback_num;
         }
      } );
   }

   acked_trx_trace_info p2p_connection::get_acked_trx_trace_info(const sysio::chain::transaction_id_type& trx_id) {
      return {};
   }

   chain::block_id_type p2p_connection::reference_block_id(const chain::block_id_type& fallback) const {
      std::lock_guard<std::mutex> lock(_peer_chain_state_lock);
      if (!is_default_block_id(_latest_fork_db_root_id)) {
         return _latest_fork_db_root_id;
      }
      if (!is_default_block_id(_latest_fork_db_head_id)) {
         return _latest_fork_db_head_id;
      }
      return fallback;
   }

   void p2p_connection::throw_if_unhealthy() const {
      if (_transport_failed.load(std::memory_order_acquire)) {
         throw std::runtime_error("P2P provider transport failed");
      }
   }

   void http_connection::connect(const chain::chain_id_type&, const chain::block_id_type&) {}

   void http_connection::disconnect() {
      int max    = provider_disconnect_wait_seconds;
      int waited = 0;
      for (uint64_t sent = _sent.load(), acknowledged = _acknowledged.load();
           sent != acknowledged && waited < max;
           sent = _sent.load(), acknowledged = _acknowledged.load()) {
         ilog("disconnect waiting on ack - sent {} | acked {} | waited {}",
              sent, acknowledged, waited);
         sleep(1);
         ++waited;
      }
      if (waited == max) {
         elog("disconnect failed to receive all acks in time - sent {} | acked {} | waited {}",
              _sent.load(), _acknowledged.load(), waited);
      }
      if (_errors.load()) {
         elog("{} errors reported during http calls, see logs", _errors.load());
      }
   }

   bool http_connection::needs_response_trace_info() {
      return is_read_only_transaction();
   }

   bool http_connection::is_read_only_transaction() {
      return _config._api_endpoint == "/v1/chain/send_read_only_transaction";
   }

   void http_connection::send_transaction(const chain::packed_transaction& trx) {
      const int         http_version = 11;
      const std::string content_type = "application/json"s;

      bool        retry                = false;
      bool        tx_rtn_failure_trace = true;
      auto        to_send              = fc::mutable_variant_object()("return_failure_trace", tx_rtn_failure_trace)("retry_trx", retry)("transaction", trx);
      std::string msg_body             = fc::json::to_string(to_send, fc::time_point::maximum());

      http_client_async::http_request_params params{_connection_thread_pool.get_executor(),
                                                    _config._peer_endpoint,
                                                    _config._port,
                                                    _config._api_endpoint,
                                                    http_version,
                                                    content_type};
      http_client_async::async_http_request(
          params, std::move(msg_body),
          [this, trx_id = trx.id()](boost::beast::error_code                                      ec,
                                    boost::beast::http::response<boost::beast::http::string_body> response) {
             trx_acknowledged(trx_id, fc::time_point::now());
             if (ec) {
                elog("http error: {}: {}", ec.value(), ec.message());
                ++_errors;
                return;
             }

             if (this->needs_response_trace_info() && response.result() == boost::beast::http::status::ok) {
                bool exception = false;
                auto exception_handler = [this, &response, &exception](const fc::exception_ptr& ex) {
                   elog("Fail to parse JSON from string: {}", response.body());
                   ++_errors;
                   exception = true;
                };
                try {
                   fc::variant resp_json = fc::json::from_string(response.body());
                   if (resp_json.is_object() && resp_json.get_object().contains("processed")) {
                      const auto& processed      = resp_json["processed"];
                      const auto& block_num      = processed["block_num"].as_uint64();
                      const auto& block_time     = processed["block_time"].as_string();
                      const auto& elapsed_time     = processed["elapsed"].as_uint64();
                      bool executed = false;
                      uint32_t    net            = 0;
                      uint32_t    cpu            = 0;
                      if (processed.get_object().contains("receipt")) {
                         const auto& receipt = processed["receipt"];
                         if (receipt.is_object()) {
                            executed = true;
                            net    = processed["net_usage"].as_int64();
                            cpu    = processed["total_cpu_usage_us"].as_int64();
                         }
                         if (executed) {
                            record_trx_info(trx_id, block_num, this->is_read_only_transaction() ? elapsed_time : cpu, net, block_time);
                         } else {
                            elog("async_http_request Transaction receipt status not executed: {}", response.body());
                         }
                      } else {
                         elog("async_http_request Transaction failed, no receipt: {}", response.body());
                      }
                   } else {
                      elog("async_http_request Transaction failed, transaction not processed: {}", response.body());
                   }
                } CATCH_AND_CALL(exception_handler)
                if (exception)
                   return;
             }

             if (!(response.result() == boost::beast::http::status::accepted ||
                   response.result() == boost::beast::http::status::ok)) {
                elog("async_http_request Failed with response http status code: {}, response: {}",
                     response.result_int(), response.body());
             }
             ++this->_acknowledged;
          });
      ++_sent;
   }

   void http_connection::record_trx_info(const sysio::chain::transaction_id_type& trx_id, uint32_t block_num,
                                         uint32_t cpu_usage_us, uint32_t net_usage_words,
                                         const std::string& block_time) {
      std::lock_guard<std::mutex> lock(_trx_info_map_lock);
      _acked_trx_trace_info_map.insert({trx_id, {true, block_num, cpu_usage_us, net_usage_words, block_time}});
   }

   acked_trx_trace_info http_connection::get_acked_trx_trace_info(const sysio::chain::transaction_id_type& trx_id) {
      acked_trx_trace_info        info;
      std::lock_guard<std::mutex> lock(_trx_info_map_lock);
      auto                        search = _acked_trx_trace_info_map.find(trx_id);
      if (search != _acked_trx_trace_info_map.end()) {
         info = search->second;
      } else {
         elog("get_acked_trx_trace_info - Acknowledged transaction trace info not found for transaction with id: {}", trx_id);
      }
      return info;
   }

   trx_provider::trx_provider(const provider_base_config& provider_config) {
      if (provider_config._peer_endpoint_type == "http") {
         _conn.emplace<http_connection>(provider_config);
         _peer_connection = &std::get<http_connection>(_conn);
      } else {
         _conn.emplace<p2p_connection>(provider_config);
         _peer_connection = &std::get<p2p_connection>(_conn);
      }
   }

   void trx_provider::setup(const chain::chain_id_type& chain_id,
                            const chain::block_id_type& last_irreversible_block_id) {
      _peer_connection->init_and_connect(chain_id, last_irreversible_block_id);
   }

   chain::block_id_type trx_provider::reference_block_id(const chain::block_id_type& fallback) const {
      return _peer_connection->reference_block_id(fallback);
   }

   void trx_provider::send(const chain::signed_transaction& trx) {
      chain::packed_transaction pt(trx);
      _peer_connection->send_transaction(pt);
      _sent_trx_data.push_back(logged_trx_data(trx.id()));
   }

   void trx_provider::log_trxs(const std::string& log_dir) {
      std::ostringstream fileName;
      fileName << log_dir << "/trx_data_output_" << getpid() << ".txt";
      std::ofstream out(fileName.str());

      for (const logged_trx_data& data : _sent_trx_data) {
         fc::time_point   acked = _peer_connection->get_trx_ack_time(data._trx_id);
         std::string      acked_str;
         fc::microseconds ack_round_trip_us;
         if (fc::time_point::min() == acked) {
            acked_str         = "NA";
            ack_round_trip_us = fc::microseconds(-1);
         } else {
            acked_str         = acked.to_iso_string();
            ack_round_trip_us = acked - data._timestamp;
         }
         out << data._trx_id.str() << "," << data._timestamp.to_iso_string() << "," << acked_str << ","
             << ack_round_trip_us.count();

         acked_trx_trace_info info = _peer_connection->get_acked_trx_trace_info(data._trx_id);
         if (info._valid) {
            out << "," << info._block_num << "," << info._cpu_usage_us << "," << info._net_usage_words << "," << info._block_time;
         }
         out << "\n";
      }
      out.close();
   }

   void trx_provider::teardown() {
      _peer_connection->cleanup_and_disconnect();
   }

   bool tps_performance_monitor::monitor_test(const tps_test_stats &stats) {
      if ((!stats.expected_sent) || (stats.last_run - stats.start_time < _spin_up_time)) {
         return true;
      }

      int32_t trxs_behind = stats.expected_sent - stats.trxs_sent;
      if (trxs_behind < 1) {
         return true;
      }

      uint32_t per_off = (100*trxs_behind) / stats.expected_sent;

      if (per_off > _max_lag_per) {
         if (_violation_start_time.has_value()) {
           auto lag_duration_us = stats.last_run - _violation_start_time.value();
           if (lag_duration_us > _max_lag_duration_us) {
               elog("Target tps lagging outside of defined limits. Terminating test");
               elog("Expected={}, Sent={}, Percent off={}, Violation start={} ",
                    stats.expected_sent, stats.trxs_sent, per_off, *_violation_start_time);
               _terminated_early = true;
               return false;
           }
         } else {
            _violation_start_time.emplace(stats.last_run);
         }
      } else {
         if (_violation_start_time.has_value()) {
            _violation_start_time.reset();
         }
      }
      return true;
   }
}
