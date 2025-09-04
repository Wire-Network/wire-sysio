
#pragma once
#include <string>
#include <vector>
#include <optional>
#include <cstdint>

namespace sysio::x509 {
  struct CAConfig {
    std::string ca_key_path;
    std::string ca_cert_path;
    std::optional<std::string> ca_key_pass;
    std::string serial_db_path;
    uint32_t default_days = 365;
  };

  void init_ca(const CAConfig& cfg);
  std::string sign_csr_pem(const std::string& csr_pem, uint32_t days);
  std::string sign_pubkey_pem(const std::string& pubkey_pem, const std::string& common_name,
                              const std::vector<std::string>& dns_sans,
                              uint32_t days);
  std::string ca_cert_pem();
}