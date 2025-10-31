#ifndef TOUCHSCREEN_INI_PARSER_HPP
#define TOUCHSCREEN_INI_PARSER_HPP

#include <string>
#include <unordered_map>
#include <optional>

namespace TouchScreen::Config {

using IniSection = std::unordered_map<std::string, std::string>;

struct IniData {
    std::unordered_map<std::string, IniSection> sections;
};

// Parse an INI file into sections/keys. Returns false on IO error.
bool LoadIni(const std::string& path, IniData& out_data);

// Save an INI file to disk. Overwrites existing file. Returns false on failure.
bool SaveIni(const std::string& path, const IniData& data);

// Helper to get a value from an INI section. Returns std::nullopt if key missing.
std::optional<std::string> GetValue(const IniData& data, const std::string& section, const std::string& key);

// Helper to set a value in the target INI section (creating section if missing).
void SetValue(IniData& data, const std::string& section, const std::string& key, const std::string& value);

// Trim whitespace from both ends of the provided string.
std::string Trim(const std::string& value);

} // namespace TouchScreen::Config

#endif // TOUCHSCREEN_INI_PARSER_HPP

