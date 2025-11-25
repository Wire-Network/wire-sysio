// SPDX-License-Identifier: MIT
#pragma once

#include <json/json.h>

namespace sysio::ethereum {

/**
 * @file utility.hpp
 * @brief Contains utility functions used across the application.
 *
 * This header defines utility functions such as loading configuration files,
 * parsing JSON data, and other helper functions that may be used throughout the codebase.
 */

/**
 * @brief Loads the configuration from a JSON file.
 * @param filename The name of the configuration file to load (default is "config.json").
 * @return An optional containing the configuration file content as a string if successful,
 *         or an empty std::optional if the file cannot be loaded.
 *
 * This function reads a JSON configuration file and returns its contents as a string.
 * It can be used to load various configuration settings, such as the Ethereum node URL.
 */
std::optional<std::string> load_config(const std::string& filename = "config.json");

} // namespace sysio::ethereum
