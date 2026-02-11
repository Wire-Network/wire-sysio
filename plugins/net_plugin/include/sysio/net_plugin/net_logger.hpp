#pragma once

#include <sysio/chain/application.hpp>
#include <fc/log/logger.hpp>
#include <string>

namespace sysio {

   inline static const std::string p2p_log_parent_name("net_plugin_impl");
   inline static fc::logger p2p_log_parent;
   inline static const std::string p2p_log_name("p2p_log");
   inline static fc::logger p2p_log;
   inline static const std::string p2p_trx_log_name("p2p_trx");
   inline static fc::logger p2p_trx_log;
   inline static const std::string p2p_blk_log_name("p2p_block");
   inline static fc::logger p2p_blk_log;
   inline static const std::string p2p_msg_log_name("p2p_message");
   inline static fc::logger p2p_msg_log;
   inline static const std::string p2p_conn_log_name("p2p_connection");
   inline static fc::logger p2p_conn_log;
   inline static std::string peer_log_format;

   template <typename Strand>
   void verify_strand_in_this_thread(const Strand& strand, const char* func, int line) {
      if( !strand.running_in_this_thread() ) {
         fc_elog( p2p_conn_log, "wrong strand: {} : line {}, exiting", func, line );
         appbase::app().quit();
      }
   }

   // peer_[x]log must be called from thread in connection strand
#define peer_dlog( LOGGER, PEER, FORMAT, ... ) \
  FC_MULTILINE_MACRO_BEGIN \
   if( (LOGGER).is_enabled( fc::log_level::debug ) ) { \
      verify_strand_in_this_thread( PEER->strand, __func__, __LINE__ ); \
      SPDLOG_LOGGER_DEBUG((LOGGER).get_agent_logger(), FC_FMT( "{}" FORMAT, PEER->get_peer_logger_string(), ##__VA_ARGS__ )); \
   } \
  FC_MULTILINE_MACRO_END

#define peer_ilog( LOGGER, PEER, FORMAT, ... ) \
  FC_MULTILINE_MACRO_BEGIN \
   if( (LOGGER).is_enabled( fc::log_level::info ) ) { \
      verify_strand_in_this_thread( PEER->strand, __func__, __LINE__ ); \
      SPDLOG_LOGGER_INFO((LOGGER).get_agent_logger(), FC_FMT( "{}" FORMAT, PEER->get_peer_logger_string(), ##__VA_ARGS__ )); \
   } \
  FC_MULTILINE_MACRO_END

#define peer_wlog( LOGGER, PEER, FORMAT, ... ) \
  FC_MULTILINE_MACRO_BEGIN \
   if( (LOGGER).is_enabled( fc::log_level::warn ) ) { \
      verify_strand_in_this_thread( PEER->strand, __func__, __LINE__ ); \
      SPDLOG_LOGGER_WARN((LOGGER).get_agent_logger(), FC_FMT( "{}" FORMAT, PEER->get_peer_logger_string(), ##__VA_ARGS__ )); \
   } \
  FC_MULTILINE_MACRO_END

#define peer_elog( LOGGER, PEER, FORMAT, ... ) \
  FC_MULTILINE_MACRO_BEGIN \
   if( (LOGGER).is_enabled( fc::log_level::error ) ) { \
      verify_strand_in_this_thread( PEER->strand, __func__, __LINE__ ); \
      SPDLOG_LOGGER_ERROR((LOGGER).get_agent_logger(), FC_FMT( "{}" FORMAT, PEER->get_peer_logger_string(), ##__VA_ARGS__ )); \
   } \
  FC_MULTILINE_MACRO_END

} // namespace sysio