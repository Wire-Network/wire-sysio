#include <fc/filesystem.hpp>
#include <fc/log/logger.hpp>
#include <fstream>
#include <sysio/ethereum/utility.hpp>

namespace sysio::ethereum {

std::optional<std::string> load_config(const std::string& filename) {

   auto          file_path = std::filesystem::absolute(filename);
   std::ifstream config_file(file_path);
   if (!config_file.is_open()) {
      wlog("Could not open config file: ${file_path}", ("file_path", file_path));
      return std::nullopt;
   }

   Json::Value config;
   config_file >> config;
   if (config.isMember("nodeUrl")) {
      return config["nodeUrl"].asString();
   }
   wlog("nodeUrl not found in config file: ${file_path}", ("file_path", file_path));

   return std::nullopt;
}

} // namespace sysio::ethereum
