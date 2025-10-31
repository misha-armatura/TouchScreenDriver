#ifndef DEVICE_HELPER_HPP
#define DEVICE_HELPER_HPP

#include <string>
#include <memory>
#include <array>
#include <vector>
#include <optional>
#include <cstdio>
#include <regex>
#include <sstream>
#include <algorithm>
#include <cctype>

#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/input.h>

namespace TouchScreen {

class DeviceHelper {
public:
    struct DeviceInfo {
        std::string path;
        int max_x = 4096;
        int max_y = 4096;
    };

    struct DetailedDeviceInfo {
        int id = -1;
        std::string name;
        std::string path;
        bool is_absolute = false;
        int max_x = 0;
        int max_y = 0;
    };

    static DeviceInfo getDeviceInfo(int deviceId) {
        DeviceInfo info;
        info.path = getDevicePathFromId(deviceId);
        
        if (!info.path.empty()) {
            std::array<char, 4096> buffer{};
            std::string cmd = "xinput list-props " + std::to_string(deviceId);
            std::unique_ptr<FILE, int(*)(FILE*)> pipe(popen(cmd.c_str(), "r"), pclose);
            if (pipe) {
                std::string output;
                while (fgets(buffer.data(), buffer.size(), pipe.get())) {
                    output += buffer.data();
                }
                std::regex areaRegex("Area \\(\\d+\\):\\s+(\\d+),\\s+(\\d+),\\s+(\\d+),\\s+(\\d+)");
                std::smatch match;
                if (std::regex_search(output, match, areaRegex) && match.size() == 5) {
                    info.max_x = std::stoi(match[3].str());
                    info.max_y = std::stoi(match[4].str());
                }
            }
        }
        
        return info;
    }

    static std::optional<std::string> getDeviceName(int deviceId) {
        std::array<char, 1024> buffer{};
        std::string cmd = "xinput list --name-only " + std::to_string(deviceId);
        std::unique_ptr<FILE, int(*)(FILE*)> pipe(popen(cmd.c_str(), "r"), pclose);
        if (!pipe) {
            return std::nullopt;
        }

        if (!fgets(buffer.data(), buffer.size(), pipe.get())) {
            return std::nullopt;
        }

        std::string name = buffer.data();
        if (!name.empty() && name.back() == '\n') {
            name.pop_back();
        }
        return name;
    }

    static std::vector<DetailedDeviceInfo> enumerateDevices() {
        std::vector<DetailedDeviceInfo> devices;
        std::unique_ptr<FILE, int(*)(FILE*)> pipe(popen("xinput list", "r"), pclose);
        if (!pipe) {
            return devices;
        }

        std::array<char, 4096> buffer{};
        while (fgets(buffer.data(), buffer.size(), pipe.get())) {
            std::string line = buffer.data();
            trim(line);
            if (line.empty() || line.find("id=") == std::string::npos) {
                continue;
            }

            DetailedDeviceInfo info;
            info.id = extractId(line);
            info.name = extractName(line);
            if (info.id < 0 || info.name.empty()) {
                continue;
            }

            info.path = getDevicePathFromId(info.id);
            auto limits = queryDeviceRanges(info.path);
            if (limits) {
                info.is_absolute = true;
                info.max_x = limits->first;
                info.max_y = limits->second;
            } else {
                // Fall back to xinput area if ioctl fails
                auto basic = getDeviceInfo(info.id);
                info.max_x = basic.max_x;
                info.max_y = basic.max_y;
                info.is_absolute = (basic.max_x > 0 && basic.max_y > 0);
            }

            devices.push_back(info);
        }

        return devices;
    }

    static std::vector<int> findRelatedDeviceIds(int deviceId, bool include_pad = false) {
        std::vector<int> related;
        related.push_back(deviceId);

        auto deviceNameOpt = getDeviceName(deviceId);
        if (!deviceNameOpt) {
            return related;
        }

        std::string familyName = computeFamilyName(*deviceNameOpt);
        if (familyName.empty()) {
            return related;
        }

        auto allDevices = enumerateDevices();
        for (const auto& device : allDevices) {
            if (device.id == deviceId) {
                continue;
            }

            auto candidateFamily = computeFamilyName(device.name);
            if (candidateFamily == familyName) {
                if (!include_pad && isPadDevice(device.name)) {
                    continue;
                }
                related.push_back(device.id);
            }
        }

        std::vector<int> unique;
        unique.reserve(related.size());
        for (int id : related) {
            if (std::find(unique.begin(), unique.end(), id) == unique.end()) {
                unique.push_back(id);
            }
        }

        if (!unique.empty() && unique.front() != deviceId) {
            auto base_it = std::find(unique.begin(), unique.end(), deviceId);
            if (base_it != unique.end()) {
                std::iter_swap(unique.begin(), base_it);
            }
        }

        return unique;
    }

private:
    static void trim(std::string& value) {
        auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
        value.erase(value.begin(),
                    std::find_if(value.begin(), value.end(),
                                 [&](char c) { return notSpace(static_cast<unsigned char>(c)); }));
        value.erase(std::find_if(value.rbegin(), value.rend(),
                                 [&](char c) { return notSpace(static_cast<unsigned char>(c)); }).base(),
                    value.end());
    }

    static int extractId(const std::string& line) {
        auto pos = line.find("id=");
        if (pos == std::string::npos) return -1;
        pos += 3;
        size_t end = pos;
        while (end < line.size() && std::isdigit(static_cast<unsigned char>(line[end]))) {
            ++end;
        }
        return std::stoi(line.substr(pos, end - pos));
    }

    static std::string extractName(const std::string& line) {
        auto idPos = line.find("id=");
        if (idPos == std::string::npos) {
            return "";
        }
        std::string name = line.substr(0, idPos);
        trim(name);
        return name;
    }

    static bool isPadDevice(const std::string& name) {
        std::string lower = toLower(name);
        return lower.find("pad") != std::string::npos;
    }

    static std::string computeFamilyName(const std::string& name) {
        std::string trimmed = name;
        trim(trimmed);
        std::string lower = toLower(trimmed);

        static const std::vector<std::string> suffixes = {
            " pen stylus",
            " pen eraser",
            " pen cursor",
            " pen pen",
            " pen pad",
            " stylus",
            " eraser",
            " cursor",
            " pad",
            " touch"
        };

        for (const auto& suffix : suffixes) {
            if (lower.size() >= suffix.size() &&
                lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0) {
                trimmed.erase(trimmed.size() - suffix.size());
                break;
            }
        }

        trim(trimmed);
        return trimmed;
    }

    static std::string toLower(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    static std::optional<std::pair<int, int>> queryDeviceRanges(const std::string& path) {
        if (path.empty()) {
            return std::nullopt;
        }

        int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            return std::nullopt;
        }

        input_absinfo abs_x {};
        input_absinfo abs_y {};

        bool has_abs = ioctl(fd, EVIOCGABS(ABS_X), &abs_x) == 0 &&
                       ioctl(fd, EVIOCGABS(ABS_Y), &abs_y) == 0;

        close(fd);

        if (!has_abs) {
            return std::nullopt;
        }

        return std::make_pair(static_cast<int>(abs_x.maximum), static_cast<int>(abs_y.maximum));
    }

    static std::string getDevicePathFromId(int deviceId) {
        std::array<char, 2048> buffer{};
        std::string result;
        
        // First verify the device exists and get its properties
        std::string cmd = "xinput list-props " + std::to_string(deviceId) + " | grep 'Device Node'";
        std::unique_ptr<FILE, int(*)(FILE*)> pipe(popen(cmd.c_str(), "r"), pclose);
        if (!pipe) {
            return "";
        }
        
        if (fgets(buffer.data(), buffer.size(), pipe.get())) {
            std::string output = buffer.data();
            std::regex deviceRegex("\"(/dev/input/event\\d+)\"");
            std::smatch match;
            if (std::regex_search(output, match, deviceRegex)) {
                return match[1].str();
            }
        }
        
        // If direct lookup failed, try getting device name and searching by path
        cmd = "xinput list --name-only " + std::to_string(deviceId);
        pipe.reset(popen(cmd.c_str(), "r"));
        if (!pipe) {
            return "";
        }
        
        std::string deviceName;
        if (fgets(buffer.data(), buffer.size(), pipe.get())) {
            deviceName = buffer.data();
            // Remove trailing newline
            if (!deviceName.empty() && deviceName.back() == '\n') {
                deviceName.pop_back();
            }
        } else {
            return "";
        }
        
        // Try both by-id and by-path directories
        const char* searchDirs[] = {"/dev/input/by-id/", "/dev/input/by-path/"};
        for (const char* dir : searchDirs) {
            cmd = "ls -l " + std::string(dir) + " 2>/dev/null | grep -i '" + deviceName + "'";
            pipe.reset(popen(cmd.c_str(), "r"));
            if (!pipe) continue;
            
            std::string linkOutput;
            while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
                linkOutput += buffer.data();
            }
            
            std::regex eventRegex("event\\d+");
            std::smatch match;
            if (std::regex_search(linkOutput, match, eventRegex)) {
                return "/dev/input/" + match.str();
            }
        }
        
        // If all else fails, try reading from xinput directly
        cmd = "xinput list-props " + std::to_string(deviceId);
        pipe.reset(popen(cmd.c_str(), "r"));
        if (!pipe) {
            return "";
        }
        
        std::string propsOutput;
        while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
            propsOutput += buffer.data();
        }
        
        std::regex deviceNodeRegex("Device Node \\(\\d+\\):\\s+\"(/dev/input/event\\d+)\"");
        std::smatch match;
        if (std::regex_search(propsOutput, match, deviceNodeRegex)) {
            return match[1].str();
        }
        
        return "";
    }
};

} // namespace TouchScreen

#endif // DEVICE_HELPER_HPP
