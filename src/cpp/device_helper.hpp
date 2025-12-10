#ifndef DEVICE_HELPER_HPP
#define DEVICE_HELPER_HPP

#include <string>
#include <memory>
#include <array>
#include <cstdio>
#include <regex>

namespace TouchScreen {

class DeviceHelper {
public:
    struct DeviceInfo {
        std::string path;
        int max_x = 4096;
        int max_y = 4096;
    };

    static DeviceInfo getDeviceInfo(int deviceId) {
        DeviceInfo info;
        info.path = getDevicePathFromId(deviceId);
        
        if (!info.path.empty()) {
            // Check for Wacom tablet area
            std::array<char, 1024> buffer;
            std::string cmd = "xinput list-props " + std::to_string(deviceId) + " | grep 'Wacom Tablet Area'";
            std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
            if (pipe) {
                if (fgets(buffer.data(), buffer.size(), pipe.get())) {
                    std::string output = buffer.data();
                    std::regex areaRegex("Area \\(\\d+\\):\\s+(\\d+),\\s+(\\d+),\\s+(\\d+),\\s+(\\d+)");
                    std::smatch match;
                    if (std::regex_search(output, match, areaRegex) && match.size() == 5) {
                        info.max_x = std::stoi(match[3].str());
                        info.max_y = std::stoi(match[4].str());
                    }
                }
            }
        }
        
        return info;
    }

private:
    static std::string getDevicePathFromId(int deviceId) {
        std::array<char, 1024> buffer;
        std::string result;
        
        // First verify the device exists and get its properties
        std::string cmd = "xinput list-props " + std::to_string(deviceId) + " | grep 'Device Node'";
        std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
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
            if (!deviceName.empty() && deviceName[deviceName.length()-1] == '\n') {
                deviceName.erase(deviceName.length()-1);
            }
        } else {
            return "";
        }
        
        // Try both by-id and by-path directories
        const char* searchDirs[] = {"/dev/input/by-id/", "/dev/input/by-path/"};
        for (const char* dir : searchDirs) {
            cmd = "ls -l " + std::string(dir) + " | grep -i '" + deviceName + "'";
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