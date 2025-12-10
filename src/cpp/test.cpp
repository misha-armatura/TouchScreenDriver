#include "touch_reader.hpp"
#include "device_helper.hpp"
#include <iostream>
#include <signal.h>
#include <atomic>
#include <string>
#include <fstream>
#include <filesystem>
#include <thread>
#include <cstring>
#include <linux/limits.h> // For PATH_MAX
#include <unistd.h> // For readlink
#include <vector>
#include <algorithm>

std::atomic<bool> running(true);
std::atomic<bool> calibration_completed(false);
std::atomic<int>  calibration_step(0);

// Store calibration values
struct CalibrationData {
    float min_x = 0.0f;
    float max_x = 1.0f; 
    float min_y = 0.0f;
    float max_y = 1.0f;
    
    // Current raw touch points for calibration
    float raw_points[4][2] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}};
    int screen_width = 800;
    int screen_height = 480;
    int offset_x = 0;
    int offset_y = 0;
};

static CalibrationData g_calibration;

// INI file operations
class ConfigFile {
public:
    static bool saveCalibration(const std::string& filename, 
                               float min_x, float max_x, float min_y, float max_y,
                               int offset_x = 0, int offset_y = 0, 
                               int screen_width = 0, int screen_height = 0) {
        std::ofstream file(filename);
        if (!file.is_open()) {
            return false;
        }
        
        file << "[Calibration]\n";
        file << "min_x=" << min_x << "\n";
        file << "max_x=" << max_x << "\n";
        file << "min_y=" << min_y << "\n";
        file << "max_y=" << max_y << "\n";
        file << "offset_x=" << offset_x << "\n";
        file << "offset_y=" << offset_y << "\n";
        
        if (screen_width > 0 && screen_height > 0) {
            file << "screen_width=" << screen_width << "\n";
            file << "screen_height=" << screen_height << "\n";
        }
        
        file << "calibrated=true\n";
        
        return true;
    }
    
    static bool loadCalibration(const std::string& filename, 
                               float& min_x, float& max_x, float& min_y, float& max_y,
                               int& offset_x, int& offset_y,
                               int& screen_width, int& screen_height) {
        std::ifstream file(filename);
        if (!file.is_open()) {
            return false;
        }
        
        std::string line;
        bool in_calibration_section = false;
        
        // Initialize with default values
        offset_x = 0;
        offset_y = 0;
        screen_width = 0;
        screen_height = 0;
        
        while (std::getline(file, line)) {
            if (line == "[Calibration]") {
                in_calibration_section = true;
                continue;
            }
            
            if (in_calibration_section) {
                if (line.find("min_x=") == 0) {
                    min_x = std::stof(line.substr(6));
                } else if (line.find("max_x=") == 0) {
                    max_x = std::stof(line.substr(6));
                } else if (line.find("min_y=") == 0) {
                    min_y = std::stof(line.substr(6));
                } else if (line.find("max_y=") == 0) {
                    max_y = std::stof(line.substr(6));
                } else if (line.find("offset_x=") == 0) {
                    offset_x = std::stoi(line.substr(9));
                } else if (line.find("offset_y=") == 0) {
                    offset_y = std::stoi(line.substr(9));
                } else if (line.find("screen_width=") == 0) {
                    screen_width = std::stoi(line.substr(13));
                } else if (line.find("screen_height=") == 0) {
                    screen_height = std::stoi(line.substr(14));
                }
            }
        }
        
        return true;
    }
    
    // Backwards compatibility version
    static bool loadCalibration(const std::string& filename, 
                               float& min_x, float& max_x, float& min_y, float& max_y) {
        int offset_x, offset_y, screen_width, screen_height;
        return loadCalibration(filename, min_x, max_x, min_y, max_y, 
                             offset_x, offset_y, screen_width, screen_height);
    }
};

// Get the directory of the executable
std::string getExePath() {
    char result[PATH_MAX];
    ssize_t count = readlink("/proc/self/exe", result, PATH_MAX);
    if (count != -1) {
        std::string path(result, count);
        return std::filesystem::path(path).parent_path().string();
    }
    return ".";
}

void signal_handler(int signal) {
    running = false;
}

// Calculate calibration values based on the four corner points
void calculateCalibration() {
    // Linear matrix calculation based on the four corner points
    // This is a simplified approach - a more robust solution would use a transformation matrix
    float tl_x = g_calibration.raw_points[0][0]; // Top-left x
    float tl_y = g_calibration.raw_points[0][1]; // Top-left y
    float tr_x = g_calibration.raw_points[1][0]; // Top-right x
    float tr_y = g_calibration.raw_points[1][1]; // Top-right y
    float br_x = g_calibration.raw_points[2][0]; // Bottom-right x
    float br_y = g_calibration.raw_points[2][1]; // Bottom-right y
    float bl_x = g_calibration.raw_points[3][0]; // Bottom-left x
    float bl_y = g_calibration.raw_points[3][1]; // Bottom-left y
    
    // Calculate ranges of raw coordinates
    float raw_width_top = tr_x - tl_x;
    float raw_width_bottom = br_x - bl_x;
    float raw_height_left = bl_y - tl_y;
    float raw_height_right = br_y - tr_y;
    
    // Use averages for better accuracy
    float raw_width = (raw_width_top + raw_width_bottom) / 2.0f;
    float raw_height = (raw_height_left + raw_height_right) / 2.0f;
    
    // Calculate offset 
    g_calibration.min_x = (tl_x + bl_x) / 2.0f; // Average of left edge x
    g_calibration.max_x = (tr_x + br_x) / 2.0f; // Average of right edge x
    g_calibration.min_y = (tl_y + tr_y) / 2.0f; // Average of top edge y
    g_calibration.max_y = (bl_y + br_y) / 2.0f; // Average of bottom edge y
}

void on_touch_event(const TouchScreen::TouchEvent& event) {
    // If we're in calibration mode, handle it specially
    if (calibration_step.load() > 0 && calibration_step.load() <= 4) {
        if (event.type == TouchScreen::EventType::TouchDown) {
            // Store the raw coordinates
            int step = calibration_step.load() - 1;
            if (event.touch_count > 0) {
                g_calibration.raw_points[step][0] = event.touches[0].raw_x;
                g_calibration.raw_points[step][1] = event.touches[0].raw_y;
                
                std::cout << "Raw coordinates: (" 
                          << g_calibration.raw_points[step][0] << ", " 
                          << g_calibration.raw_points[step][1] << ")" << std::endl;
            }
            
            calibration_step++;
            std::cout << "Calibration point " << calibration_step.load()-1 << " recorded." << std::endl;
            
            if (calibration_step.load() > 4) {
                calculateCalibration();
                std::cout << "Calibration completed!" << std::endl;
                std::cout << "Calibration values: min_x=" << g_calibration.min_x 
                          << ", max_x=" << g_calibration.max_x 
                          << ", min_y=" << g_calibration.min_y 
                          << ", max_y=" << g_calibration.max_y << std::endl;
                calibration_completed = true;
            } else {
                std::cout << "Please touch the " 
                          << (calibration_step.load() == 1 ? "top-left" : 
                              calibration_step.load() == 2 ? "top-right" : 
                              calibration_step.load() == 3 ? "bottom-right" : "bottom-left") 
                          << " corner of your screen." << std::endl;
            }
            return;
        }
    }
    
    // Normal event display for non-calibration mode
    // Convert enum to string for display
    const char* event_names[] = {
        "TouchDown", "TouchUp", "TouchMove", "SwipeLeft", "SwipeRight", 
        "SwipeUp", "SwipeDown", "PinchIn", "PinchOut", "LongPress", 
        "DoubleTap", "Rotate"
    };
    
    std::cout << "Event: " << event_names[static_cast<int>(event.type)] 
              << " at (" << event.x << ", " << event.y << ") "
              << "Touches: " << event.touch_count;
    
    if (event.type == TouchScreen::EventType::SwipeLeft || 
        event.type == TouchScreen::EventType::SwipeRight ||
        event.type == TouchScreen::EventType::SwipeUp ||
        event.type == TouchScreen::EventType::SwipeDown ||
        event.type == TouchScreen::EventType::PinchIn ||
        event.type == TouchScreen::EventType::PinchOut) {
        std::cout << " Value: " << event.value;
    }
    
    std::cout << std::endl;
    
    // Print active touches
    if (event.touch_count > 0) {
        std::cout << "  Active touches:" << std::endl;
        for (const auto& touch : event.touches) {
            std::cout << "    ID: " << touch.tracking_id
                      << " Position: (" << touch.x << ", " << touch.y << ")"
                      << " Raw: (" << touch.raw_x << ", " << touch.raw_y << ")"
                      << std::endl;
        }
    }
}

void printUsage(const char* programName) {
    // Extract just the filename part without path
    const char* basename = strrchr(programName, '/');
    const char* name = basename ? basename + 1 : programName;
    
    std::cout << "Usage: " << name << " [options]" << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  -h, --help            Display this help message" << std::endl;
    std::cout << "  -c, --calibrate       Run calibration procedure" << std::endl;
    std::cout << "  -l, --load            Load calibration from INI file" << std::endl;
    std::cout << "  -d, --device <path>   Specify touch device path" << std::endl;
    std::cout << "  --device-id <id>      Use specific xinput device ID" << std::endl;
    std::cout << "  -r, --resolution <w>x<h>  Specify screen resolution (default: 1920x1080)" << std::endl;
    std::cout << "  -m, --monitor <index> Specify monitor index (0 to N-1 for specific monitor, -1=full desktop)" << std::endl;
    std::cout << std::endl;
    std::cout << "Calibration Modes:" << std::endl;
    std::cout << "  1. Full Desktop Mode:" << std::endl;
    std::cout << "     Use '--monitor -1' to calibrate across all monitors" << std::endl;
    std::cout << "     Example: " << name << " --calibrate --monitor -1" << std::endl;
    std::cout << std::endl;
    std::cout << "  2. Single Monitor Mode:" << std::endl;
    std::cout << "     Use '--monitor <index>' to calibrate for a specific monitor only" << std::endl;
    std::cout << "     Example: " << name << " --calibrate --monitor 0  (for first monitor)" << std::endl;
    std::cout << "     Example: " << name << " --calibrate --monitor 1  (for second monitor)" << std::endl;
    std::cout << std::endl;
    std::cout << "Examples:" << std::endl;
    std::cout << "  " << name << " --calibrate --resolution 1024x768" << std::endl;
    std::cout << "  " << name << " --load --monitor 1" << std::endl;
    std::cout << "  " << name << " --device /dev/input/event2" << std::endl;
}

int main(int argc, char* argv[]) {
    // Register signal handler
    signal(SIGINT, signal_handler);
    
    // Parse command line arguments
    bool run_calibration = false;
    bool load_calibration = false;
    std::string device_path;
    int device_id = -1;
    int screen_width = 1920;  // Default to a standard single monitor resolution
    int screen_height = 1080;
    int monitor_index = -1;   // -1 means use full desktop, 0-N means specific monitor
    
    struct MonitorInfo {
        int width;
        int height;
        int offset_x;
        int offset_y;
        std::string name;
    };
    
    // Function to detect monitors dynamically
    auto detectMonitors = []() -> std::vector<MonitorInfo> {
        std::vector<MonitorInfo> monitors;
        
        // Use xrandr to get monitor information
        FILE* pipe = popen("xrandr --listmonitors 2>/dev/null | grep -E '^\s*[0-9]+:' | awk '{print $3,$4}'", "r");
        if (pipe) {
            char buffer[1024];
            while (fgets(buffer, sizeof(buffer), pipe)) {
                MonitorInfo monitor;
                int w, h, x, y;
                char name[256];
                
                // Parse format like "1920/518x1080/324+0+0 DP-4"
                if (sscanf(buffer, "%d/%*dx%d/%*d+%d+%d %s", &w, &h, &x, &y, name) == 5) {
                    monitor.width = w;
                    monitor.height = h;
                    monitor.offset_x = x;
                    monitor.offset_y = y;
                    monitor.name = name;
                    monitors.push_back(monitor);
                }
            }
            pclose(pipe);
        }
        
        // Fallback to single monitor if detection fails
        if (monitors.empty()) {
            MonitorInfo fallback;
            fallback.width = 1920;
            fallback.height = 1080;
            fallback.offset_x = 0;
            fallback.offset_y = 0;
            fallback.name = "Default Monitor";
            monitors.push_back(fallback);
        }
        
        return monitors;
    };
    
    auto monitors = detectMonitors();
    const int MONITOR_COUNT = monitors.size();
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printUsage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--calibrate") == 0) {
            run_calibration = true;
        } else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--load") == 0) {
            load_calibration = true;
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--device") == 0) {
            if (i + 1 < argc) {
                device_path = argv[i + 1];
                i++;
            } else {
                std::cerr << "Error: --device requires a path argument" << std::endl;
                return 1;
            }
        } else if (strcmp(argv[i], "--device-id") == 0) {
            if (i + 1 < argc) {
                device_id = std::stoi(argv[i + 1]);
                auto deviceInfo = TouchScreen::DeviceHelper::getDeviceInfo(device_id);
                if (!deviceInfo.path.empty()) {
                    device_path = deviceInfo.path;
                    std::cout << "Found device path for ID " << device_id << ": " << device_path << std::endl;
                    std::cout << "Device coordinate range: 0-" << deviceInfo.max_x << " x 0-" << deviceInfo.max_y << std::endl;
                    
                    // Update calibration range for this device
                    g_calibration.min_x = 0;
                    g_calibration.max_x = deviceInfo.max_x;
                    g_calibration.min_y = 0;
                    g_calibration.max_y = deviceInfo.max_y;
                    g_calibration.screen_width = screen_width;
                    g_calibration.screen_height = screen_height;
                } else {
                    std::cerr << "Error: Could not find device path for ID " << device_id << std::endl;
                    return 1;
                }
                i++;
            } else {
                std::cerr << "Error: --device-id requires a numeric argument" << std::endl;
                return 1;
            }
        } else if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--resolution") == 0) {
            if (i + 1 < argc) {
                std::string res = argv[i + 1];
                size_t x_pos = res.find('x');
                if (x_pos != std::string::npos) {
                    screen_width = std::stoi(res.substr(0, x_pos));
                    screen_height = std::stoi(res.substr(x_pos + 1));
                    g_calibration.screen_width = screen_width;
                    g_calibration.screen_height = screen_height;
                } else {
                    std::cerr << "Error: Invalid resolution format. Use WIDTHxHEIGHT (e.g., 800x480)" << std::endl;
                    return 1;
                }
                i++;
            } else {
                std::cerr << "Error: --resolution requires a value" << std::endl;
                return 1;
            }
        } else if (strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--monitor") == 0) {
            if (i + 1 < argc) {
                monitor_index = std::stoi(argv[i + 1]);
                if (monitor_index >= 0 && monitor_index < MONITOR_COUNT) {
                    screen_width = monitors[monitor_index].width;
                    screen_height = monitors[monitor_index].height;
                    g_calibration.screen_width = screen_width;
                    g_calibration.screen_height = screen_height;
                    printf("Using monitor %d: %s (%dx%d at +%d+%d)\n", 
                        monitor_index, monitors[monitor_index].name.c_str(),
                        monitors[monitor_index].width, monitors[monitor_index].height,
                        monitors[monitor_index].offset_x, monitors[monitor_index].offset_y);
                } else if (monitor_index == -1) {
                    // Calculate full desktop dimensions
                    int max_x = 0, max_y = 0;
                    for (const auto& mon : monitors) {
                        max_x = std::max(max_x, mon.offset_x + mon.width);
                        max_y = std::max(max_y, mon.offset_y + mon.height);
                    }
                    screen_width = max_x;
                    screen_height = max_y;
                    g_calibration.screen_width = screen_width;
                    g_calibration.screen_height = screen_height;
                    printf("Using full desktop: %dx%d\n", screen_width, screen_height);
                } else {
                    std::cerr << "Error: Invalid monitor index. Valid values are 0-" << (MONITOR_COUNT-1) << " or -1 for full desktop" << std::endl;
                    return 1;
                }
                i++;
            } else {
                std::cerr << "Error: --monitor requires an index" << std::endl;
                return 1;
            }
        }
    }
    
    // Show detected monitor setup
    printf("Detected multi-monitor setup:\n");
    for (int i = 0; i < MONITOR_COUNT; i++) {
        printf("  Monitor %d: %s - %dx%d at position +%d+%d%s\n", 
            i, monitors[i].name.c_str(), monitors[i].width, monitors[i].height, 
            monitors[i].offset_x, monitors[i].offset_y,
            (monitor_index == i) ? " [SELECTED]" : "");
    }
    
    // Create touch reader
    TouchScreen::TouchReader reader;
    
    // Set event callback
    reader.SetEventCallback(on_touch_event);
    
    std::cout << "Touch Reader Test" << std::endl;
    
    // Get calibration file path in the same directory as the executable
    std::string calibration_file = getExePath() + "/touch_calibration.ini";
    
    // Add monitor index to calibration file if specific monitor is selected
    if (monitor_index >= 0) {
        calibration_file = getExePath() + "/touch_calibration_mon" + std::to_string(monitor_index) + ".ini";
    }
    
    // Start the device
    bool started = false;
    if (!device_path.empty()) {
        std::cout << "Using specified device: " << device_path << std::endl;
        started = reader.Start(device_path);
        
        // Apply calibration if we have device-specific settings
        if (device_id >= 0) {
            reader.SetCalibration(g_calibration.min_x, g_calibration.max_x,
                                g_calibration.min_y, g_calibration.max_y,
                                g_calibration.screen_width, g_calibration.screen_height);
        }
    } else {
        std::cout << "Trying to auto-detect touch device..." << std::endl;
        started = reader.StartAuto();
    }
    
    if (!started) {
        std::cout << "Failed to initialize touch screen. Make sure you have the right permissions." << std::endl;
        return 1;
    }
    
    std::cout << "Touch screen found: " << reader.GetSelectedDevice() << std::endl;
    
    // If a specific monitor was selected, apply appropriate offsets
    if (monitor_index >= 0 && monitor_index < MONITOR_COUNT) {
        int x_offset = monitors[monitor_index].offset_x;
        int y_offset = monitors[monitor_index].offset_y;
        
        // We'll adjust this after calibration
        g_calibration.offset_x = x_offset;
        g_calibration.offset_y = y_offset;
        printf("Will apply monitor offset: (%d, %d)\n", x_offset, y_offset);
    }
    
    // Handle calibration
    if (run_calibration) {
        std::cout << "\n===== CALIBRATION MODE =====\n" << std::endl;
        std::cout << "Screen resolution: " << screen_width << "x" << screen_height << std::endl;
        std::cout << "Please touch the top-left corner of your screen." << std::endl;
        
        calibration_step = 1;
        
        // Calibration loop
        while (running && !calibration_completed) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        if (calibration_completed) {
            // Save calibration data
            float min_x = g_calibration.min_x;
            float max_x = g_calibration.max_x;
            float min_y = g_calibration.min_y;
            float max_y = g_calibration.max_y;
            
            if (ConfigFile::saveCalibration(calibration_file, min_x, max_x, min_y, max_y,
                                           g_calibration.offset_x, g_calibration.offset_y,
                                           g_calibration.screen_width, g_calibration.screen_height)) {
                std::cout << "Calibration data saved to " << calibration_file << std::endl;
            } else {
                std::cerr << "Failed to save calibration data!" << std::endl;
            }
            
            // Apply monitor offsets if needed
            if (monitor_index >= 0 && monitor_index < MONITOR_COUNT) {
                int x_offset = monitors[monitor_index].offset_x;
                int y_offset = monitors[monitor_index].offset_y;
                reader.SetCalibrationOffset(x_offset, y_offset);
                printf("Applied monitor offset: (%d, %d)\n", x_offset, y_offset);
            }
        }
        
        reader.Stop();
        return 0;
    }
    
    // Load calibration if requested
    if (load_calibration) {
        float min_x = 0, max_x = 0, min_y = 0, max_y = 0;
        int offset_x = 0, offset_y = 0;
        
        if (ConfigFile::loadCalibration(calibration_file, min_x, max_x, min_y, max_y,
                                       offset_x, offset_y, screen_width, screen_height)) {
            std::cout << "Loaded calibration data from " << calibration_file << std::endl;
            std::cout << "Calibration values: min_x=" << min_x << ", max_x=" << max_x 
                      << ", min_y=" << min_y << ", max_y=" << max_y << std::endl;
            
            // Apply calibration through the reader
            reader.SetCalibration(min_x, max_x, min_y, max_y, screen_width, screen_height);
            
            // Apply monitor offsets if needed
            if (monitor_index >= 0 && monitor_index < MONITOR_COUNT) {
                int x_offset = monitors[monitor_index].offset_x;
                int y_offset = monitors[monitor_index].offset_y;
                reader.SetCalibrationOffset(x_offset, y_offset);
                printf("Applied monitor offset: (%d, %d)\n", x_offset, y_offset);
            }
        } else {
            std::cout << "No calibration data found or failed to load from " << calibration_file << std::endl;
        }
    }
    
    std::cout << "Press Ctrl+C to exit" << std::endl;
    
    // Main loop
    while (running) {
        // Do some important work
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    // Stop reader
    reader.Stop();
    std::cout << "Touch reader stopped." << std::endl;
    
    return 0;
} 