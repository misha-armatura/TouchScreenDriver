#include "ini_parser.hpp"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace TouchScreen::Config {

namespace {

bool IsComment(const std::string& line) {
    for (char ch : line) {
        if (ch == '#') return true;
        if (ch == ';') return true;
        if (!std::isspace(static_cast<unsigned char>(ch))) {
            return false;
        }
    }
    return true;
}

} // namespace

std::string Trim(const std::string& value) {
    size_t start = 0;
    size_t end = value.size();

    while (start < end && std::isspace(static_cast<unsigned char>(value[start]))) {
        ++start;
    }
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(start, end - start);
}

bool LoadIni(const std::string& path, IniData& out_data) {
    std::ifstream input(path);
    if (!input.is_open()) {
        return false;
    }

    std::string line;
    std::string current_section = "default";

    while (std::getline(input, line)) {
        std::string trimmed = Trim(line);
        if (trimmed.empty() || IsComment(trimmed)) {
            continue;
        }

        if (trimmed.front() == '[' && trimmed.back() == ']') {
            current_section = Trim(trimmed.substr(1, trimmed.size() - 2));
            continue;
        }

        auto delimiter_pos = trimmed.find('=');
        if (delimiter_pos == std::string::npos) {
            continue; // skip malformed line
        }

        std::string key = Trim(trimmed.substr(0, delimiter_pos));
        std::string value = Trim(trimmed.substr(delimiter_pos + 1));
        out_data.sections[current_section][key] = value;
    }

    return true;
}

bool SaveIni(const std::string& path, const IniData& data) {
    std::ofstream output(path);
    if (!output.is_open()) {
        return false;
    }

    for (const auto& [section_name, section] : data.sections) {
        if (!section_name.empty()) {
            output << "[" << section_name << "]\n";
        }
        for (const auto& [key, value] : section) {
            output << key << "=" << value << "\n";
        }
        output << "\n";
    }

    return output.good();
}

std::optional<std::string> GetValue(const IniData& data, const std::string& section, const std::string& key) {
    auto section_it = data.sections.find(section);
    if (section_it == data.sections.end()) {
        return std::nullopt;
    }
    auto key_it = section_it->second.find(key);
    if (key_it == section_it->second.end()) {
        return std::nullopt;
    }
    return key_it->second;
}

void SetValue(IniData& data, const std::string& section, const std::string& key, const std::string& value) {
    data.sections[section][key] = value;
}

} // namespace TouchScreen::Config

