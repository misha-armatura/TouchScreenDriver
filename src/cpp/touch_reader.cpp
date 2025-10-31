#include "touch_reader.hpp"
#include <cstring>
#include <cmath>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <iostream>
#include <errno.h>
#include <dirent.h>
#include <vector>
#include <string>
#include <array>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cctype>

#include "ini_parser.hpp"

// Maximum number of slots to track touches
#define MAX_SLOTS 10
// Maximum number of events in the queue
#define MAX_EVENTS 32

// Gesture thresholds from the C implementation
#define SWIPE_MIN_DISTANCE 50
#define PINCH_THRESHOLD 20
#define LONG_PRESS_THRESHOLD_MS 500
#define DOUBLE_TAP_THRESHOLD_MS 300

// Default calibration values
#define DEFAULT_MIN_X 0
#define DEFAULT_MAX_X 40640
#define DEFAULT_MIN_Y 0
#define DEFAULT_MAX_Y 30480
#define DEFAULT_SCREEN_WIDTH 800
#define DEFAULT_SCREEN_HEIGHT 480

// Debug flag - set to true for detailed output
#define DEBUG_OUTPUT 1

// Debug printing macro
#define DEBUG_PRINT(fmt, ...) \
    do { if (DEBUG_OUTPUT) std::cerr << "[TouchReader] " << fmt << std::endl; } while (0)

namespace TouchScreen {

// Implementation class for TouchReader
class TouchReaderImpl {
public:
    TouchReaderImpl();
    ~TouchReaderImpl();
    
    bool Start(const std::string& device);
    bool StartAuto();
    void Stop();
    
    void SetEventCallback(EventCallback callback);
    
    int GetTouchCount() const;
    bool GetTouchCoordinates(int index, int& x, int& y) const;
    bool GetRawTouchCoordinates(int index, int& x, int& y) const;
    std::vector<TouchPoint> GetActiveTouches() const;
    
    bool GetNextEvent(TouchEvent& event);
    bool WaitForEvent(TouchEvent& event, int timeout_ms);
    void ClearEvents();
    
    void SetCalibration(int min_x, int max_x, int min_y, int max_y, int screen_width, int screen_height);
    void SetAffineCalibration(const std::array<double, 6>& matrix, int screen_width, int screen_height);
    void SetCalibrationMargin(double margin_percent);
    void SetCalibrationOffset(int x_offset, int y_offset);
    Calibration GetCalibration() const;
    bool LoadCalibration(const std::string& filename);
    bool SaveCalibration(const std::string& filename);
    bool RunCalibration(int screen_width, int screen_height);
    
    std::string GetSelectedDevice() const;

    bool EnableMitm(bool enable, bool grab_source);

private:
    // Touch data
    struct TouchData {
        int tracking_id = -1;
        int raw_x = 0;
        int raw_y = 0;
        int x = 0;
        int y = 0;
        int start_x = 0;
        int start_y = 0;
        int64_t timestamp = 0;
    };
    
    // Reader thread
    void ReaderThread();
    void DetectGestures();
    void ApplyCalibration(int raw_x, int raw_y, int& x, int& y);
    int64_t GetTimestampMs();
    int CalculateDistance(const TouchData& t1, const TouchData& t2);
    
    // Add event to the queue
    void AddEvent(EventType type, int touch_count, int x, int y, int value);
    
    // Member variables
    int fd_ = -1;
    std::string selected_device_;
    std::atomic<bool> running_{false};
    std::thread reader_thread_;
    
    // Touch tracking
    mutable std::mutex touch_mutex_;
    TouchData touches_[MAX_SLOTS];
    int current_slot_ = 0;
    
    // Event queue
    std::mutex event_mutex_;
    std::condition_variable event_cv_;
    std::queue<TouchEvent> event_queue_;
    
    // Calibration
    Calibration calibration_;
    
    // Callback
    EventCallback event_callback_ = nullptr;
    
    // Gesture detection state
    int64_t last_tap_time_ = 0;
    int last_tap_x_ = 0;
    int last_tap_y_ = 0;
    int prev_touch_count_ = 0;
    int prev_distance_ = 0;
    bool gesture_tracking_ = false;
    TouchData gesture_start_touches_[MAX_SLOTS];
    
    // New member variable for StartAuto()
    bool is_mouse_device_ = false;

    // uinput / MITM
    int uinput_fd_ = -1;
    bool mitm_enabled_ = false;
    bool grabbed_source_ = false;

    // helpers for uinput
    bool InitUinput();
    void DestroyUinput();
    void EmitToUinput(int touch_count, int x, int y);

    // device capabilities
    bool device_has_btn_touch_ = false;
    void DetectDeviceCapabilities();
};

// Helper function to get current timestamp in milliseconds
int64_t TouchReaderImpl::GetTimestampMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

// Constructor
TouchReaderImpl::TouchReaderImpl() {
    // Initialize touches array
    for (int i = 0; i < MAX_SLOTS; ++i) {
        touches_[i].tracking_id = -1;
    }
    
    // Initialize calibration with defaults
    calibration_.mode = CalibrationMode::MinMax;
    calibration_.min_x = static_cast<double>(DEFAULT_MIN_X);
    calibration_.max_x = static_cast<double>(DEFAULT_MAX_X);
    calibration_.min_y = static_cast<double>(DEFAULT_MIN_Y);
    calibration_.max_y = static_cast<double>(DEFAULT_MAX_Y);
    calibration_.screen_width = DEFAULT_SCREEN_WIDTH;
    calibration_.screen_height = DEFAULT_SCREEN_HEIGHT;
    
    // Calculate factors
    calibration_.x_factor = static_cast<double>(calibration_.screen_width) / std::max(1.0, calibration_.max_x - calibration_.min_x);
    calibration_.y_factor = static_cast<double>(calibration_.screen_height) / std::max(1.0, calibration_.max_y - calibration_.min_y);
    calibration_.x_offset = 0;
    calibration_.y_offset = 0;
    calibration_.margin_percent = 0.0;
    calibration_.affine = {1.0, 0.0, 0.0, 0.0, 1.0, 0.0};
}

// Destructor
TouchReaderImpl::~TouchReaderImpl() {
    Stop();
}

// Apply calibration to raw touchscreen coordinates
void TouchReaderImpl::ApplyCalibration(int raw_x, int raw_y, int& x, int& y) {
    double raw_dx = static_cast<double>(raw_x);
    double raw_dy = static_cast<double>(raw_y);

    if (calibration_.mode == CalibrationMode::Affine) {
        double mapped_x = calibration_.affine[0] * raw_dx +
                          calibration_.affine[1] * raw_dy +
                          calibration_.affine[2];
        double mapped_y = calibration_.affine[3] * raw_dx +
                          calibration_.affine[4] * raw_dy +
                          calibration_.affine[5];

        mapped_x += static_cast<double>(calibration_.x_offset);
        mapped_y += static_cast<double>(calibration_.y_offset);

        double min_screen_x = static_cast<double>(calibration_.x_offset);
        double max_screen_x = min_screen_x + std::max(0, calibration_.screen_width - 1);
        double min_screen_y = static_cast<double>(calibration_.y_offset);
        double max_screen_y = min_screen_y + std::max(0, calibration_.screen_height - 1);

        mapped_x = std::clamp(mapped_x, min_screen_x, max_screen_x);
        mapped_y = std::clamp(mapped_y, min_screen_y, max_screen_y);

        x = static_cast<int>(std::lround(mapped_x));
        y = static_cast<int>(std::lround(mapped_y));
        return;
    }

    const double min_x = calibration_.min_x;
    const double max_x = calibration_.max_x;
    const double min_y = calibration_.min_y;
    const double max_y = calibration_.max_y;

    double range_x = max_x - min_x;
    double range_y = max_y - min_y;

    if (range_x <= 0.0) {
        range_x = 1.0;
    }
    if (range_y <= 0.0) {
        range_y = 1.0;
    }

    double clamped_raw_x = std::clamp(raw_dx, min_x, max_x);
    double clamped_raw_y = std::clamp(raw_dy, min_y, max_y);

    double u = (clamped_raw_x - min_x) / range_x;
    double v = (clamped_raw_y - min_y) / range_y;

    u = std::clamp(u, 0.0, 1.0);
    v = std::clamp(v, 0.0, 1.0);

    double screen_x = u * std::max(0, calibration_.screen_width - 1);
    double screen_y = v * std::max(0, calibration_.screen_height - 1);

    screen_x += static_cast<double>(calibration_.x_offset);
    screen_y += static_cast<double>(calibration_.y_offset);

    double min_screen_x = static_cast<double>(calibration_.x_offset);
    double max_screen_x = min_screen_x + std::max(0, calibration_.screen_width - 1);
    double min_screen_y = static_cast<double>(calibration_.y_offset);
    double max_screen_y = min_screen_y + std::max(0, calibration_.screen_height - 1);

    screen_x = std::clamp(screen_x, min_screen_x, max_screen_x);
    screen_y = std::clamp(screen_y, min_screen_y, max_screen_y);

    x = static_cast<int>(std::lround(screen_x));
    y = static_cast<int>(std::lround(screen_y));
}

// Start touch reader with specific device
bool TouchReaderImpl::Start(const std::string& device) {
    if (running_) {
        return false;
    }
    
    // Open the touch device
    fd_ = open(device.c_str(), O_RDONLY);
    if (fd_ < 0) {
        return false;
    }
    
    selected_device_ = device;
    DetectDeviceCapabilities();
    running_ = true;
    
    // Start reader thread
    reader_thread_ = std::thread(&TouchReaderImpl::ReaderThread, this);
    
    return true;
}

// Calculate distance between two touch points
int TouchReaderImpl::CalculateDistance(const TouchData& t1, const TouchData& t2) {
    int dx = t1.x - t2.x;
    int dy = t1.y - t2.y;
    return (int)sqrt(dx * dx + dy * dy);
}

// Add event to the queue
void TouchReaderImpl::AddEvent(EventType type, int touch_count, int x, int y, int value) {
    TouchEvent evt;
    evt.type = type;
    evt.touch_count = touch_count;
    evt.x = x;
    evt.y = y;
    evt.value = value;
    evt.timestamp = GetTimestampMs();
    
    // Copy current active touches
    {
        std::lock_guard<std::mutex> lock(touch_mutex_);
        for (int i = 0; i < MAX_SLOTS; ++i) {
            if (touches_[i].tracking_id >= 0) {
                TouchPoint point;
                point.tracking_id = touches_[i].tracking_id;
                point.raw_x = touches_[i].raw_x;
                point.raw_y = touches_[i].raw_y;
                point.x = touches_[i].x;
                point.y = touches_[i].y;
                point.start_x = touches_[i].start_x;
                point.start_y = touches_[i].start_y;
                point.timestamp = touches_[i].timestamp;
                evt.touches.push_back(point);
            }
        }
    }
    
    // Add to queue and notify waiting threads
    {
        std::lock_guard<std::mutex> lock(event_mutex_);
        event_queue_.push(evt);
        
        // Limit queue size
        if (event_queue_.size() > MAX_EVENTS) {
            event_queue_.pop();
        }
    }
    
    event_cv_.notify_one();
    
    // Call callback if registered
    if (event_callback_) {
        event_callback_(evt);
    }
}

// Function for getting all devices from /dev/input
std::vector<std::string> GetAllInputDevices() {
    std::vector<std::string> devices;
    DIR* dir = opendir("/dev/input");
    
    if (dir == nullptr) {
        DEBUG_PRINT("Failed to open /dev/input directory");
        return devices;
    }
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        // Skip empty names and dots ("." "..")
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        std::string device_path = "/dev/input/";
        device_path += entry->d_name;
        devices.push_back(device_path);
    }
    
    closedir(dir);
    DEBUG_PRINT("Found " << devices.size() << " input devices");
    return devices;
}

// Auto-detect touch device
bool TouchReaderImpl::StartAuto() {
    if (running_) {
        return false;
    }
    
    // Get list of all input devices
    std::vector<std::string> all_devices = GetAllInputDevices();
    const int num_devices = all_devices.size();
    
    if (num_devices == 0) {
        DEBUG_PRINT("No input devices found");
        return false;
    }
    
    struct input_event ev;
    std::vector<int> fds(num_devices, -1);

    DEBUG_PRINT("Auto-detecting touchscreen device...");
    
    // Try to open every device and check permissions
    for (int i = 0; i < num_devices; ++i) {
        fds[i] = open(all_devices[i].c_str(), O_RDONLY | O_NONBLOCK);
        if (fds[i] < 0) {
            DEBUG_PRINT("Failed to open " << all_devices[i] << ": " << strerror(errno));
        } else {
            DEBUG_PRINT("Successfully opened " << all_devices[i] << " with fd " << fds[i]);
        }
    }

    // For the first, check mouse devices, that can be a touchscreen
    for (int i = 0; i < num_devices; ++i) {
        if (fds[i] >= 0 && all_devices[i].find("mouse") != std::string::npos) {
            DEBUG_PRINT("Trying to use mouse device: " << all_devices[i]);
            
            // Close all others file descriptors
            for (int j = 0; j < num_devices; ++j) {
                if (j != i && fds[j] >= 0) {
                    close(fds[j]);
                }
            }
            
            is_mouse_device_ = true;
            bool result = Start(all_devices[i]);
            if (result) {
                DEBUG_PRINT("Using mouse device: " << all_devices[i]);
                return true;
            }
            
            // If fails, reopen file descriptors again
            for (int j = 0; j < num_devices; ++j) {
                if (j != i && fds[j] < 0) {
                    fds[j] = open(all_devices[j].c_str(), O_RDONLY | O_NONBLOCK);
                }
            }
        }
    }
    
    // Recheck event devices again
    for (int i = 0; i < num_devices; ++i) {
        if (fds[i] >= 0 && all_devices[i].find("event") != std::string::npos) {
            DEBUG_PRINT("Trying to use event device: " << all_devices[i]);
            
            // Close all others file descriptors
            for (int j = 0; j < num_devices; ++j) {
                if (j != i && fds[j] >= 0) {
                    close(fds[j]);
                }
            }
            
            is_mouse_device_ = false;
            bool result = Start(all_devices[i]);
            if (result) {
                DEBUG_PRINT("Using event device: " << all_devices[i]);
                return true;
            }
            
            // If fails, reopen file descriptors again
            for (int j = 0; j < num_devices; ++j) {
                if (j != i && fds[j] < 0) {
                    fds[j] = open(all_devices[j].c_str(), O_RDONLY | O_NONBLOCK);
                }
            }
        }
    }
    
    // Check all other devices
    for (int i = 0; i < num_devices; ++i) {
        if (fds[i] >= 0 && 
            all_devices[i].find("mouse") == std::string::npos && 
            all_devices[i].find("event") == std::string::npos) {
            
            DEBUG_PRINT("Trying to use other device: " << all_devices[i]);
            
            // Close all others file descriptors
            for (int j = 0; j < num_devices; ++j) {
                if (j != i && fds[j] >= 0) {
                    close(fds[j]);
                }
            }
            
            is_mouse_device_ = false;
            bool result = Start(all_devices[i]);
            if (result) {
                DEBUG_PRINT("Using other device: " << all_devices[i]);
                return true;
            }
        }
    }
    
    // Close all file descriptors
    for (int i = 0; i < num_devices; ++i) {
        if (fds[i] >= 0) {
            close(fds[i]);
        }
    }
    
    DEBUG_PRINT("Could not find a suitable touchscreen device");
    return false;
}

// Stop touch reader
void TouchReaderImpl::Stop() {
    if (running_) {
        running_ = false;
        if (reader_thread_.joinable()) {
            reader_thread_.join();
        }
        if (fd_ >= 0) {
            if (grabbed_source_) {
                ioctl(fd_, EVIOCGRAB, 0);
                grabbed_source_ = false;
            }
            close(fd_);
            fd_ = -1;
        }
        DestroyUinput();
        mitm_enabled_ = false;
    }
}

// Set event callback
void TouchReaderImpl::SetEventCallback(EventCallback callback) {
    event_callback_ = callback;
}

// Get number of active touches
int TouchReaderImpl::GetTouchCount() const {
    int count = 0;
    std::lock_guard<std::mutex> lock(touch_mutex_);
    for (int i = 0; i < MAX_SLOTS; ++i) {
        if (touches_[i].tracking_id >= 0) count++;
    }
    return count;
}

// Get touch coordinates for an index
bool TouchReaderImpl::GetTouchCoordinates(int index, int& x, int& y) const {
    std::lock_guard<std::mutex> lock(touch_mutex_);
    int count = 0;
    for (int i = 0; i < MAX_SLOTS; ++i) {
        if (touches_[i].tracking_id >= 0) {
            if (count == index) {
                x = touches_[i].x;
                y = touches_[i].y;
                return true;
            }
            count++;
        }
    }
    return false;
}

// Get raw touch coordinates for an index
bool TouchReaderImpl::GetRawTouchCoordinates(int index, int& x, int& y) const {
    std::lock_guard<std::mutex> lock(touch_mutex_);
    int count = 0;
    for (int i = 0; i < MAX_SLOTS; ++i) {
        if (touches_[i].tracking_id >= 0) {
            if (count == index) {
                x = touches_[i].raw_x;
                y = touches_[i].raw_y;
                return true;
            }
            count++;
        }
    }
    return false;
}

// Get all active touches
std::vector<TouchPoint> TouchReaderImpl::GetActiveTouches() const {
    std::vector<TouchPoint> result;
    std::lock_guard<std::mutex> lock(touch_mutex_);
    for (int i = 0; i < MAX_SLOTS; ++i) {
        if (touches_[i].tracking_id >= 0) {
            TouchPoint point;
            point.tracking_id = touches_[i].tracking_id;
            point.raw_x = touches_[i].raw_x;
            point.raw_y = touches_[i].raw_y;
            point.x = touches_[i].x;
            point.y = touches_[i].y;
            point.start_x = touches_[i].start_x;
            point.start_y = touches_[i].start_y;
            point.timestamp = touches_[i].timestamp;
            result.push_back(point);
        }
    }
    return result;
}

// Get next event from the queue (non-blocking)
bool TouchReaderImpl::GetNextEvent(TouchEvent& event) {
    std::lock_guard<std::mutex> lock(event_mutex_);
    if (event_queue_.empty()) {
        return false;
    }
    
    event = event_queue_.front();

    event_queue_.pop();
    return true;
}

// Wait for next event with timeout
bool TouchReaderImpl::WaitForEvent(TouchEvent& event, int timeout_ms) {
    if (!running_) {
        return false;  // Don't wait if not running
    }
    
    try {
        std::unique_lock<std::mutex> lock(event_mutex_);
        
        // First check if there's already an event available
        if (!event_queue_.empty()) {
            event = event_queue_.front();

            event_queue_.pop();
            return true;
        }
        
        // Don't wait if queue is empty and timeout is 0
        if (timeout_ms == 0) {
            return false;
        }
        
        bool result = false;
        if (timeout_ms < 0) {
            // Wait with a maximum timeout to avoid hanging indefinitely
            constexpr int MAX_WAIT_MS = 1000; // 1 second max wait time
            result = event_cv_.wait_for(lock, std::chrono::milliseconds(MAX_WAIT_MS), 
                                 [this] { return !event_queue_.empty() || !running_; });
        } else {
            // Wait with specified timeout
            result = event_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), 
                                 [this] { return !event_queue_.empty() || !running_; });
        }
        
        // Check if we're still running and got an event
        if (!running_) {
            return false;
        }
        
        if (result && !event_queue_.empty()) {
            event = event_queue_.front();

            event_queue_.pop();
            return true;
        }
        
        return false;
    } 
    catch (const std::exception& e) {
        printf("Exception in WaitForEvent: %s\n", e.what());
        return false;
    }
    catch (...) {
        printf("Unknown exception in WaitForEvent\n");
        return false;
    }
}

// Clear all pending events
void TouchReaderImpl::ClearEvents() {
    std::lock_guard<std::mutex> lock(event_mutex_);
    std::queue<TouchEvent> empty;
    std::swap(event_queue_, empty);
}

// Set calibration parameters
void TouchReaderImpl::SetCalibration(int min_x, int max_x, int min_y, int max_y, int screen_width, int screen_height) {
    std::lock_guard<std::mutex> lock(touch_mutex_);
    
    calibration_.mode = CalibrationMode::MinMax;
    calibration_.min_x = static_cast<double>(min_x);
    calibration_.max_x = static_cast<double>(max_x);
    calibration_.min_y = static_cast<double>(min_y);
    calibration_.max_y = static_cast<double>(max_y);
    calibration_.screen_width = screen_width;
    calibration_.screen_height = screen_height;

    double range_x = calibration_.max_x - calibration_.min_x;
    double range_y = calibration_.max_y - calibration_.min_y;

    if (range_x <= 0.0) {
        range_x = 1.0;
    }
    if (range_y <= 0.0) {
        range_y = 1.0;
    }

    calibration_.x_factor = static_cast<double>(screen_width) / range_x;
    calibration_.y_factor = static_cast<double>(screen_height) / range_y;
    calibration_.margin_percent = 0.0;
    calibration_.affine = {1.0, 0.0, 0.0, 0.0, 1.0, 0.0};
}

void TouchReaderImpl::SetAffineCalibration(const std::array<double, 6>& matrix, int screen_width, int screen_height) {
    std::lock_guard<std::mutex> lock(touch_mutex_);

    calibration_.mode = CalibrationMode::Affine;
    calibration_.affine = matrix;
    calibration_.screen_width = screen_width;
    calibration_.screen_height = screen_height;
    calibration_.x_factor = 1.0;
    calibration_.y_factor = 1.0;
}

void TouchReaderImpl::SetCalibrationMargin(double margin_percent) {
    std::lock_guard<std::mutex> lock(touch_mutex_);
    calibration_.margin_percent = margin_percent;
}

// Set calibration offsets
void TouchReaderImpl::SetCalibrationOffset(int x_offset, int y_offset) {
    std::lock_guard<std::mutex> lock(touch_mutex_);
    calibration_.x_offset = x_offset;
    calibration_.y_offset = y_offset;
}

// Get current calibration
Calibration TouchReaderImpl::GetCalibration() const {
    return calibration_;
}

// Load calibration from file
bool TouchReaderImpl::LoadCalibration(const std::string& filename) {
    Config::IniData data;
    if (Config::LoadIni(filename, data)) {
        auto get_double = [](const std::optional<std::string>& value, double fallback) {
            if (!value) return fallback;
            try {
                return std::stod(*value);
            } catch (...) {
                return fallback;
            }
        };
        auto get_int = [](const std::optional<std::string>& value, int fallback) {
            if (!value) return fallback;
            try {
                return std::stoi(*value);
            } catch (...) {
                return fallback;
            }
        };

        auto mode_str = Config::GetValue(data, "Calibration", "mode").value_or("minmax");
        std::string mode_lower = mode_str;
        std::transform(mode_lower.begin(), mode_lower.end(), mode_lower.begin(), [](unsigned char c) { return std::tolower(c); });

        int screen_width = get_int(Config::GetValue(data, "Calibration", "screen_width"), calibration_.screen_width);
        int screen_height = get_int(Config::GetValue(data, "Calibration", "screen_height"), calibration_.screen_height);
        int offset_x = get_int(Config::GetValue(data, "Calibration", "offset_x"), 0);
        int offset_y = get_int(Config::GetValue(data, "Calibration", "offset_y"), 0);
        double margin = get_double(Config::GetValue(data, "Calibration", "margin_percent"), 0.0);

        if (mode_lower == "affine") {
            std::array<double, 6> matrix{
                get_double(Config::GetValue(data, "Affine", "m0"), calibration_.affine[0]),
                get_double(Config::GetValue(data, "Affine", "m1"), calibration_.affine[1]),
                get_double(Config::GetValue(data, "Affine", "m2"), calibration_.affine[2]),
                get_double(Config::GetValue(data, "Affine", "m3"), calibration_.affine[3]),
                get_double(Config::GetValue(data, "Affine", "m4"), calibration_.affine[4]),
                get_double(Config::GetValue(data, "Affine", "m5"), calibration_.affine[5])
            };

            SetAffineCalibration(matrix, screen_width, screen_height);
            calibration_.x_offset = offset_x;
            calibration_.y_offset = offset_y;
            calibration_.margin_percent = margin;
            return true;
        }

        // Default to min/max mode
        double min_x = get_double(Config::GetValue(data, "Calibration", "min_x"), calibration_.min_x);
        double max_x = get_double(Config::GetValue(data, "Calibration", "max_x"), calibration_.max_x);
        double min_y = get_double(Config::GetValue(data, "Calibration", "min_y"), calibration_.min_y);
        double max_y = get_double(Config::GetValue(data, "Calibration", "max_y"), calibration_.max_y);

        SetCalibration(static_cast<int>(std::lround(min_x)), static_cast<int>(std::lround(max_x)),
                       static_cast<int>(std::lround(min_y)), static_cast<int>(std::lround(max_y)),
                       screen_width, screen_height);

        // Preserve precise values
        calibration_.min_x = min_x;
        calibration_.max_x = max_x;
        calibration_.min_y = min_y;
        calibration_.max_y = max_y;
        calibration_.x_offset = offset_x;
        calibration_.y_offset = offset_y;
        calibration_.margin_percent = margin;

        double range_x = calibration_.max_x - calibration_.min_x;
        double range_y = calibration_.max_y - calibration_.min_y;
        if (range_x <= 0.0) range_x = 1.0;
        if (range_y <= 0.0) range_y = 1.0;
        calibration_.x_factor = static_cast<double>(screen_width) / range_x;
        calibration_.y_factor = static_cast<double>(screen_height) / range_y;

        return true;
    }

    // Legacy plain format fallback
    FILE* f = fopen(filename.c_str(), "r");
    if (!f) {
        return false;
    }

    int min_x, max_x, min_y, max_y, screen_width, screen_height, x_offset, y_offset;
    if (fscanf(f, "%d %d %d %d %d %d %d %d",
               &min_x, &max_x, &min_y, &max_y,
               &screen_width, &screen_height,
               &x_offset, &y_offset) == 8) {
        fclose(f);
        SetCalibration(min_x, max_x, min_y, max_y, screen_width, screen_height);
        SetCalibrationOffset(x_offset, y_offset);
        return true;
    }

    fclose(f);
    return false;
}

// Save calibration to file
bool TouchReaderImpl::SaveCalibration(const std::string& filename) {
    Config::IniData data;
    auto to_string_precise = [](double value) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(6) << value;
        return oss.str();
    };

    Config::SetValue(data, "Calibration", "mode",
        calibration_.mode == CalibrationMode::Affine ? "affine" : "minmax");
    Config::SetValue(data, "Calibration", "min_x", to_string_precise(calibration_.min_x));
    Config::SetValue(data, "Calibration", "max_x", to_string_precise(calibration_.max_x));
    Config::SetValue(data, "Calibration", "min_y", to_string_precise(calibration_.min_y));
    Config::SetValue(data, "Calibration", "max_y", to_string_precise(calibration_.max_y));
    Config::SetValue(data, "Calibration", "screen_width", std::to_string(calibration_.screen_width));
    Config::SetValue(data, "Calibration", "screen_height", std::to_string(calibration_.screen_height));
    Config::SetValue(data, "Calibration", "offset_x", std::to_string(calibration_.x_offset));
    Config::SetValue(data, "Calibration", "offset_y", std::to_string(calibration_.y_offset));
    Config::SetValue(data, "Calibration", "margin_percent", to_string_precise(calibration_.margin_percent));

    if (calibration_.mode == CalibrationMode::Affine) {
        Config::SetValue(data, "Affine", "m0", to_string_precise(calibration_.affine[0]));
        Config::SetValue(data, "Affine", "m1", to_string_precise(calibration_.affine[1]));
        Config::SetValue(data, "Affine", "m2", to_string_precise(calibration_.affine[2]));
        Config::SetValue(data, "Affine", "m3", to_string_precise(calibration_.affine[3]));
        Config::SetValue(data, "Affine", "m4", to_string_precise(calibration_.affine[4]));
        Config::SetValue(data, "Affine", "m5", to_string_precise(calibration_.affine[5]));
    }

    // Metadata for debugging
    Config::SetValue(data, "Metadata", "saved_with", "touch_reader");

    return Config::SaveIni(filename, data);
}

// Get the selected device name
std::string TouchReaderImpl::GetSelectedDevice() const {
    return selected_device_;
}

bool TouchReaderImpl::InitUinput() {
    if (uinput_fd_ >= 0) return true;
    uinput_fd_ = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (uinput_fd_ < 0) {
        DEBUG_PRINT("Failed to open /dev/uinput: " << strerror(errno));
        return false;
    }

    // Enable event types
    ioctl(uinput_fd_, UI_SET_EVBIT, EV_KEY);
    ioctl(uinput_fd_, UI_SET_KEYBIT, BTN_TOUCH);
    ioctl(uinput_fd_, UI_SET_EVBIT, EV_ABS);
    ioctl(uinput_fd_, UI_SET_ABSBIT, ABS_X);
    ioctl(uinput_fd_, UI_SET_ABSBIT, ABS_Y);
    ioctl(uinput_fd_, UI_SET_EVBIT, EV_SYN);

    struct uinput_user_dev uidev {};
    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "touch_reader_calibrated");
    uidev.id.bustype = BUS_USB;
    uidev.id.vendor  = 0x1234;
    uidev.id.product = 0x5678;
    uidev.id.version = 1;

    // Set ABS ranges to screen dimensions
    uidev.absmin[ABS_X] = 0;
    uidev.absmax[ABS_X] = calibration_.screen_width - 1;
    uidev.absmin[ABS_Y] = 0;
    uidev.absmax[ABS_Y] = calibration_.screen_height - 1;

    if (write(uinput_fd_, &uidev, sizeof(uidev)) < 0) {
        DEBUG_PRINT("Failed to write uinput_user_dev: " << strerror(errno));
        close(uinput_fd_);
        uinput_fd_ = -1;
        return false;
    }
    if (ioctl(uinput_fd_, UI_DEV_CREATE) < 0) {
        DEBUG_PRINT("Failed to create uinput device: " << strerror(errno));
        close(uinput_fd_);
        uinput_fd_ = -1;
        return false;
    }
    return true;
}

void TouchReaderImpl::DestroyUinput() {
    if (uinput_fd_ >= 0) {
        ioctl(uinput_fd_, UI_DEV_DESTROY);
        close(uinput_fd_);
        uinput_fd_ = -1;
    }
}

static void write_ev(int fd, uint16_t type, uint16_t code, int32_t value) {
    struct input_event ev {};
    // Kernel ignores timestamp for injected events; zero it for portability
    memset(&ev.time, 0, sizeof(ev.time));
    ev.type = type;
    ev.code = code;
    ev.value = value;
    ssize_t result = write(fd, &ev, sizeof(ev));
    if (result != sizeof(ev)) {
        DEBUG_PRINT("Failed to write event: " << strerror(errno));
    }
}

void TouchReaderImpl::EmitToUinput(int touch_count, int x, int y) {
    if (uinput_fd_ < 0) return;
    // Simple single-touch emulation: BTN_TOUCH + ABS_X/ABS_Y
    if (touch_count > 0) {
        write_ev(uinput_fd_, EV_KEY, BTN_TOUCH, 1);
        write_ev(uinput_fd_, EV_ABS, ABS_X, x);
        write_ev(uinput_fd_, EV_ABS, ABS_Y, y);
    } else {
        write_ev(uinput_fd_, EV_KEY, BTN_TOUCH, 0);
    }
    write_ev(uinput_fd_, EV_SYN, SYN_REPORT, 0);
}

bool TouchReaderImpl::EnableMitm(bool enable, bool grab_source) {
    if (enable) {
        if (!InitUinput()) return false;
        if (grab_source && fd_ >= 0 && !grabbed_source_) {
            if (ioctl(fd_, EVIOCGRAB, 1) == 0) {
                grabbed_source_ = true;
            } else {
                DEBUG_PRINT("Warning: EVIOCGRAB failed: " << strerror(errno));
            }
        }
        mitm_enabled_ = true;
        return true;
    } else {
        if (grabbed_source_ && fd_ >= 0) {
            ioctl(fd_, EVIOCGRAB, 0);
            grabbed_source_ = false;
        }
        DestroyUinput();
        mitm_enabled_ = false;
        return true;
    }
}

void TouchReaderImpl::DetectDeviceCapabilities() {
    device_has_btn_touch_ = false;
    if (fd_ < 0) return;

    unsigned long evbit[(EV_MAX + 7) / 8] = {0};
    if (ioctl(fd_, EVIOCGBIT(0, sizeof(evbit)), evbit) < 0) {
        return;
    }
    auto test_bit = [](unsigned long *arr, int bit) {
        return (arr[bit / (8 * sizeof(unsigned long))] >> (bit % (8 * sizeof(unsigned long)))) & 1UL;
    };

    if (!test_bit(evbit, EV_KEY)) return;

    unsigned long keybit[(KEY_MAX + 7) / 8] = {0};
    if (ioctl(fd_, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit) < 0) {
        return;
    }
    device_has_btn_touch_ = ((keybit[BTN_TOUCH / (8 * sizeof(unsigned long))] >> (BTN_TOUCH % (8 * sizeof(unsigned long)))) & 1UL) != 0;
}

// Main reader thread
void TouchReaderImpl::ReaderThread() {
    struct input_event ev;
    bool updated = false;
    
    // Initialize touch tracking
    for (int i = 0; i < MAX_SLOTS; ++i) {
        touches_[i].tracking_id = -1;
    }
    
    // Check if this is a mouse device
    bool is_mouse_device = selected_device_.find("mouse") != std::string::npos;
    DEBUG_PRINT("Reader thread started. Device: " << selected_device_ << ", is_mouse_device: " << (is_mouse_device ? "true" : "false"));
    
    // Mouse protocol handling variables
    unsigned char mouse_data[3];
    int mouse_bytes_read = 0;
    
    while (running_) {
        if (is_mouse_device) {
            // Handle PS/2 mouse protocol
            unsigned char byte;
            int res = read(fd_, &byte, 1);
            if (res <= 0) continue;
            
            // Store byte in the mouse data buffer
            mouse_data[mouse_bytes_read++] = byte;
            
            // PS/2 mouse protocol uses 3-byte packets
            if (mouse_bytes_read == 3) {
                std::lock_guard<std::mutex> lock(touch_mutex_);
                
                // First byte contains button state and sign bits
                bool left_button = (mouse_data[0] & 0x01) != 0;
                bool right_button = (mouse_data[0] & 0x02) != 0;
                
                // Extract movement values (with sign extension)
                int dx = mouse_data[1] - ((mouse_data[0] & 0x10) ? 256 : 0);
                int dy = mouse_data[2] - ((mouse_data[0] & 0x20) ? 256 : 0);
                
                // Convert buttons to touch events
                if (left_button) {
                    // Ensure we have an active touch or create one
                    if (touches_[0].tracking_id < 0) {
                        touches_[0].tracking_id = 0;
                        touches_[0].raw_x = 2048; // Center of screen
                        touches_[0].raw_y = 2048; // Center of screen
                        ApplyCalibration(touches_[0].raw_x, touches_[0].raw_y, 
                                        touches_[0].x, touches_[0].y);
                        touches_[0].start_x = touches_[0].x;
                        touches_[0].start_y = touches_[0].y;
                        touches_[0].timestamp = GetTimestampMs();
                    }
                    
                    // Update touch position
                    touches_[0].raw_x += dx;
                    touches_[0].raw_y -= dy; // Invert Y axis
                    
                    // Keep within bounds
                    if (touches_[0].raw_x < 0) touches_[0].raw_x = 0;
                    if (touches_[0].raw_x > 4095) touches_[0].raw_x = 4095;
                    if (touches_[0].raw_y < 0) touches_[0].raw_y = 0;
                    if (touches_[0].raw_y > 4095) touches_[0].raw_y = 4095;
                    
                    ApplyCalibration(touches_[0].raw_x, touches_[0].raw_y, 
                                    touches_[0].x, touches_[0].y);
                } else if (touches_[0].tracking_id >= 0) {
                    // Release the touch
                    touches_[0].tracking_id = -1;
                }
                
                // Process the updated touch state
                updated = true;
                mouse_bytes_read = 0;
            }
        } else {
            // Standard event device handling (original code)
            if (read(fd_, &ev, sizeof(ev)) != sizeof(ev)) continue;
            
            if (ev.type == EV_ABS) {
                std::lock_guard<std::mutex> lock(touch_mutex_);
                switch (ev.code) {
                    case ABS_MT_SLOT:
                        current_slot_ = ev.value;
                        break;
                        
                    case ABS_MT_TRACKING_ID:
                        touches_[current_slot_].tracking_id = ev.value;
                        if (ev.value >= 0) {
                            touches_[current_slot_].timestamp = GetTimestampMs();
                            touches_[current_slot_].start_x = touches_[current_slot_].x;
                            touches_[current_slot_].start_y = touches_[current_slot_].y;
                        }
                        updated = true;
                        break;
                        
                    case ABS_MT_POSITION_X:
                        touches_[current_slot_].raw_x = ev.value;
                        ApplyCalibration(ev.value, touches_[current_slot_].raw_y, 
                                        touches_[current_slot_].x, touches_[current_slot_].y);
                        updated = true;
                        break;
                        
                    case ABS_MT_POSITION_Y:
                        touches_[current_slot_].raw_y = ev.value;
                        ApplyCalibration(touches_[current_slot_].raw_x, ev.value, 
                                        touches_[current_slot_].x, touches_[current_slot_].y);
                        updated = true;
                        break;
                    // Single-touch absolute axes (e.g., tablets, pens)
                    case ABS_X:
                        touches_[0].raw_x = ev.value;
                        ApplyCalibration(touches_[0].raw_x, touches_[0].raw_y,
                                        touches_[0].x, touches_[0].y);
                        updated = true;
                        break;
                    case ABS_Y:
                        touches_[0].raw_y = ev.value;
                        ApplyCalibration(touches_[0].raw_x, touches_[0].raw_y,
                                        touches_[0].x, touches_[0].y);
                        updated = true;
                        break;
                }
            }
            
            // Handle simpler mouse-like devices
            if (ev.type == EV_KEY) {
                // Prefer BTN_TOUCH for true contact; use BTN_TOOL_PEN only as fallback when BTN_TOUCH is absent
                bool is_contact_key = false;
                if (ev.code == BTN_TOUCH) {
                    is_contact_key = true;
                } else if (!device_has_btn_touch_ && (ev.code == BTN_TOOL_PEN || ev.code == BTN_LEFT)) {
                    is_contact_key = true;
                }

                if (is_contact_key) {
                    std::lock_guard<std::mutex> lock(touch_mutex_);
                    if (ev.value) { // Button down
                        if (touches_[0].tracking_id < 0) {
                            touches_[0].tracking_id = 0;
                            touches_[0].timestamp = GetTimestampMs();
                            touches_[0].start_x = touches_[0].x;
                            touches_[0].start_y = touches_[0].y;
                        }
                    } else { // Button up
                        touches_[0].tracking_id = -1;
                    }
                    updated = true;
                }
            }
            
            if (ev.type == EV_REL) {
                std::lock_guard<std::mutex> lock(touch_mutex_);
                // Only process relative events if we have an active touch
                if (touches_[0].tracking_id >= 0) {
                    if (ev.code == REL_X) {
                        touches_[0].raw_x += ev.value;
                        ApplyCalibration(touches_[0].raw_x, touches_[0].raw_y,
                                        touches_[0].x, touches_[0].y);
                        updated = true;
                    } else if (ev.code == REL_Y) {
                        touches_[0].raw_y += ev.value;
                        ApplyCalibration(touches_[0].raw_x, touches_[0].raw_y,
                                        touches_[0].x, touches_[0].y);
                        updated = true;
                    }
                }
            }
        }
        
        // SYN_REPORT - process accumulated updates or do it for mouse devices
        if ((is_mouse_device && updated) || 
            (!is_mouse_device && ev.type == EV_SYN && ev.code == SYN_REPORT && updated)) {
            DetectGestures();
            updated = false;
        }
    }
}

// Detect gestures from touch input
void TouchReaderImpl::DetectGestures() {
    int touch_count = 0;
    int64_t current_time;
    int primary_x = 0, primary_y = 0;
    TouchData *t1 = nullptr, *t2 = nullptr;
    
    // Count active touches and find primary
    for (int i = 0; i < MAX_SLOTS; ++i) {
        if (touches_[i].tracking_id >= 0) {
            touch_count++;
            if (!t1) t1 = &touches_[i];
            else if (!t2) t2 = &touches_[i];
            
            primary_x += touches_[i].x;
            primary_y += touches_[i].y;
        }
    }
    
    if (touch_count > 0) {
        primary_x /= touch_count;
        primary_y /= touch_count;
    }
    
    current_time = GetTimestampMs();
    
    // Detect touch_down event
    if (touch_count > 0 && prev_touch_count_ == 0) {
        AddEvent(EventType::TouchDown, touch_count, primary_x, primary_y, 0);
        if (mitm_enabled_) EmitToUinput(touch_count, primary_x, primary_y);
        
        // Start gesture tracking
        gesture_tracking_ = true;
        memcpy(gesture_start_touches_, touches_, sizeof(touches_));
        
        // Long press detection setup
        for (int i = 0; i < MAX_SLOTS; ++i) {
            if (touches_[i].tracking_id >= 0) {
                touches_[i].start_x = touches_[i].x;
                touches_[i].start_y = touches_[i].y;
                touches_[i].timestamp = current_time;
            }
        }
    }
    
    // Detect touch_up event
    if (touch_count == 0 && prev_touch_count_ > 0) {
        AddEvent(EventType::TouchUp, 0, primary_x, primary_y, 0);
        if (mitm_enabled_) EmitToUinput(0, primary_x, primary_y);
        gesture_tracking_ = false;
        
        // Check for long press
        for (int i = 0; i < MAX_SLOTS; ++i) {
            if (gesture_start_touches_[i].tracking_id >= 0) {
                int dx = abs(gesture_start_touches_[i].x - gesture_start_touches_[i].start_x);
                int dy = abs(gesture_start_touches_[i].y - gesture_start_touches_[i].start_y);
                
                // If didn't move much and held for threshold time
                if (dx < 20 && dy < 20 && 
                    (current_time - gesture_start_touches_[i].timestamp) >= LONG_PRESS_THRESHOLD_MS) {
                    AddEvent(EventType::LongPress, 1, gesture_start_touches_[i].x, gesture_start_touches_[i].y, 0);
                }
            }
        }
        
        // Double tap detection
        if (prev_touch_count_ == 1) {
            int dx = abs(primary_x - last_tap_x_);
            int dy = abs(primary_y - last_tap_y_);
            
            if (dx < 30 && dy < 30 && 
                (current_time - last_tap_time_) < DOUBLE_TAP_THRESHOLD_MS) {
                AddEvent(EventType::DoubleTap, 1, primary_x, primary_y, 0);
            }
            
            last_tap_time_ = current_time;
            last_tap_x_ = primary_x;
            last_tap_y_ = primary_y;
        }
        
        // Swipe detection
        if (prev_touch_count_ == 1) {
            for (int i = 0; i < MAX_SLOTS; ++i) {
                if (gesture_start_touches_[i].tracking_id >= 0) {
                    int dx = gesture_start_touches_[i].x - gesture_start_touches_[i].start_x;
                    int dy = gesture_start_touches_[i].y - gesture_start_touches_[i].start_y;
                    
                    if (abs(dx) > SWIPE_MIN_DISTANCE && abs(dx) > abs(dy) * 2) {
                        if (dx > 0) {
                            AddEvent(EventType::SwipeRight, 1, primary_x, primary_y, dx);
                        } else {
                            AddEvent(EventType::SwipeLeft, 1, primary_x, primary_y, -dx);
                        }
                    } else if (abs(dy) > SWIPE_MIN_DISTANCE && abs(dy) > abs(dx) * 2) {
                        if (dy > 0) {
                            AddEvent(EventType::SwipeDown, 1, primary_x, primary_y, dy);
                        } else {
                            AddEvent(EventType::SwipeUp, 1, primary_x, primary_y, -dy);
                        }
                    }
                    break;
                }
            }
        }
    }
    
    // Detect touch_move
    if (touch_count > 0 && touch_count == prev_touch_count_) {
        AddEvent(EventType::TouchMove, touch_count, primary_x, primary_y, 0);
        if (mitm_enabled_) EmitToUinput(touch_count, primary_x, primary_y);
    }
    
    // Multi-touch gestures
    if (touch_count == 2 && prev_touch_count_ == 2 && t1 && t2) {
        // Pinch gesture
        int current_distance = CalculateDistance(*t1, *t2);
        
        if (prev_distance_ > 0 && abs(current_distance - prev_distance_) > PINCH_THRESHOLD) {
            if (current_distance > prev_distance_) {
                AddEvent(EventType::PinchOut, 2, primary_x, primary_y, current_distance - prev_distance_);
            } else {
                AddEvent(EventType::PinchIn, 2, primary_x, primary_y, prev_distance_ - current_distance);
            }
        }
        
        prev_distance_ = current_distance;
        
        // Rotation detection could be added here
    }
    
    prev_touch_count_ = touch_count;
}

// Run a touchscreen calibration routine
bool TouchReaderImpl::RunCalibration(int screen_width, int screen_height) {
    if (!running_) return false;
    
    // Store the original event callback so we can restore it later
    auto original_callback = event_callback_;
    
    // Create arrays to store the calibration points
    int points[4][2] = {{0}};  // Will store raw touch points
    int targets[4][2] = {
        {20, 20},                                   // Top-left
        {screen_width - 20, 20},                    // Top-right
        {screen_width - 20, screen_height - 20},    // Bottom-right
        {20, screen_height - 20}                    // Bottom-left
    };
    
    printf("Starting touch screen calibration...\n");
    printf("Screen resolution: %d x %d\n", screen_width, screen_height);
    printf("Please touch each corner when prompted.\n");
    
    std::atomic<bool> point_received(false);
    std::atomic<int> current_point(0);
    
    // Set up a temporary callback to capture calibration touches
    SetEventCallback([&](const TouchScreen::TouchEvent& event) {
        if (event.type == EventType::TouchDown && event.touch_count > 0 && 
            current_point.load() < 4 && !point_received.load()) {
            
            // Store the touch coordinates
            points[current_point][0] = event.touches[0].raw_x;
            points[current_point][1] = event.touches[0].raw_y;
            
            printf("Received point %d: Raw(%d, %d)\n", 
                current_point.load() + 1, 
                points[current_point][0], 
                points[current_point][1]);
            
            point_received = true;
        }
    });
    
    // Guide the user through the calibration process
    for (int i = 0; i < 4; i++) {
        current_point = i;
        point_received = false;
        
        // Prompt for the current point
        const char* corner_names[] = {"top-left", "top-right", "bottom-right", "bottom-left"};
        printf("Please touch the %s corner of your screen.\n", corner_names[i]);
        
        // Wait for touch input with timeout
        int timeout_count = 0;
        while (!point_received.load() && timeout_count < 150 && running_) { // 15 second timeout
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            timeout_count++;
        }
        
        if (!point_received.load()) {
            if (!running_) {
                printf("Calibration aborted.\n");
                // Restore the original callback
                SetEventCallback(original_callback);
                return false;
            }
            
            printf("No touch detected for point %d. Using default value.\n", i + 1);
            // Use reasonable defaults if no touch was detected
            switch (i) {
                case 0: points[i][0] = 0;    points[i][1] = 0;    break; // Top-left
                case 1: points[i][0] = 4095; points[i][1] = 0;    break; // Top-right
                case 2: points[i][0] = 4095; points[i][1] = 4095; break; // Bottom-right
                case 3: points[i][0] = 0;    points[i][1] = 4095; break; // Bottom-left
            }
        }
        
        // Add a small delay between points
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    
    // Restore the original callback
    SetEventCallback(original_callback);
    
    // Compute calibration parameters
    int min_x = (points[0][0] + points[3][0]) / 2;  // Average of left edge
    int max_x = (points[1][0] + points[2][0]) / 2;  // Average of right edge
    int min_y = (points[0][1] + points[1][1]) / 2;  // Average of top edge
    int max_y = (points[2][1] + points[3][1]) / 2;  // Average of bottom edge
    
    // Safety checks to prevent division by zero
    if (min_x == max_x) {
        printf("Warning: X coordinates are identical, using default range.\n");
        min_x = 0;
        max_x = 4095;
    }
    
    if (min_y == max_y) {
        printf("Warning: Y coordinates are identical, using default range.\n");
        min_y = 0;
        max_y = 4095;
    }
    
    // Set calibration parameters
    SetCalibration(min_x, max_x, min_y, max_y, screen_width, screen_height);
    
    // Log calibration for debugging
    printf("Calibration complete!\n");
    printf("Calibration values: min_x=%d, max_x=%d, min_y=%d, max_y=%d, screen_width=%d, screen_height=%d\n",
           min_x, max_x, min_y, max_y, screen_width, screen_height);
    
    return true;
}

// TouchReader class implementation using pimpl idiom
TouchReader::TouchReader() : impl_(new TouchReaderImpl()) {
}

TouchReader::~TouchReader() {
}

bool TouchReader::Start(const std::string& device) {
    return impl_->Start(device);
}

bool TouchReader::StartAuto() {
    return impl_->StartAuto();
}

void TouchReader::Stop() {
    impl_->Stop();
}

void TouchReader::SetEventCallback(EventCallback callback) {
    impl_->SetEventCallback(callback);
}

int TouchReader::GetTouchCount() const {
    return impl_->GetTouchCount();
}

bool TouchReader::GetTouchCoordinates(int index, int& x, int& y) const {
    return impl_->GetTouchCoordinates(index, x, y);
}

bool TouchReader::GetRawTouchCoordinates(int index, int& x, int& y) const {
    return impl_->GetRawTouchCoordinates(index, x, y);
}

std::vector<TouchPoint> TouchReader::GetActiveTouches() const {
    return impl_->GetActiveTouches();
}

bool TouchReader::GetNextEvent(TouchEvent& event) {
    return impl_->GetNextEvent(event);
}

bool TouchReader::WaitForEvent(TouchEvent& event, int timeout_ms) {
    return impl_->WaitForEvent(event, timeout_ms);
}

void TouchReader::ClearEvents() {
    impl_->ClearEvents();
}

void TouchReader::SetCalibration(int min_x, int max_x, int min_y, int max_y, int screen_width, int screen_height) {
    impl_->SetCalibration(min_x, max_x, min_y, max_y, screen_width, screen_height);
}

void TouchReader::SetAffineCalibration(const std::array<double, 6>& matrix, int screen_width, int screen_height) {
    impl_->SetAffineCalibration(matrix, screen_width, screen_height);
}

void TouchReader::SetCalibrationMargin(double margin_percent) {
    impl_->SetCalibrationMargin(margin_percent);
}

void TouchReader::SetCalibrationOffset(int x_offset, int y_offset) {
    impl_->SetCalibrationOffset(x_offset, y_offset);
}

Calibration TouchReader::GetCalibration() const {
    return impl_->GetCalibration();
}

bool TouchReader::LoadCalibration(const std::string& filename) {
    return impl_->LoadCalibration(filename);
}

bool TouchReader::SaveCalibration(const std::string& filename) {
    return impl_->SaveCalibration(filename);
}

bool TouchReader::RunCalibration(int screen_width, int screen_height) {
    return impl_->RunCalibration(screen_width, screen_height);
}

std::string TouchReader::GetSelectedDevice() const {
    return impl_->GetSelectedDevice();
}

bool TouchReader::EnableMitm(bool enable, bool grab_source) {
    return impl_->EnableMitm(enable, grab_source);
}

} // namespace TouchScreen 

// C-compatible API implementation
extern "C" {

// Opaque handle structure
struct TouchReaderHandleStruct {
    TouchScreen::TouchReader* reader;
    TouchEventCallbackFn callback;
    void* user_data;
};

// Event callback bridge function
void c_event_callback(const TouchScreen::TouchEvent& event, TouchReaderHandle handle) {
    if (handle && handle->callback) {
        int event_type = static_cast<int>(event.type);
        handle->callback(event_type, event.touch_count, event.x, event.y, event.value, handle->user_data);
    }
}

// Create/destroy functions
TouchReaderHandle touch_reader_create() {
    auto handle = new TouchReaderHandleStruct();
    handle->reader = new TouchScreen::TouchReader();
    handle->callback = nullptr;
    handle->user_data = nullptr;
    return handle;
}

void touch_reader_destroy(TouchReaderHandle handle) {
    if (handle) {
        delete handle->reader;
        delete handle;
    }
}

// Open/close functions
int touch_reader_open(TouchReaderHandle handle, const char* device) {
    if (!handle || !device) return -1;
    return handle->reader->Start(device) ? 0 : -1;
}

// Start/stop functions
int touch_reader_start(TouchReaderHandle handle, const char* device) {
    if (!handle || !device) return -1;
    return handle->reader->Start(device) ? 0 : -1;
}

int touch_reader_start_auto(TouchReaderHandle handle) {
    if (!handle) return -1;
    return handle->reader->StartAuto() ? 0 : -1;
}

void touch_reader_stop(TouchReaderHandle handle) {
    if (handle) {
        handle->reader->Stop();
    }
}

void touch_reader_close(TouchReaderHandle handle) {
    if (handle) {
        handle->reader->Stop();
    }
}

// Set callback function
void touch_reader_set_callback(TouchReaderHandle handle, TouchEventCallbackFn callback, void* user_data) {
    if (!handle) return;
    
    handle->callback = callback;
    handle->user_data = user_data;
    
    if (callback) {
        handle->reader->SetEventCallback([handle](const TouchScreen::TouchEvent& event) {
            if (handle->callback) {
                int event_type = static_cast<int>(event.type);
                handle->callback(event_type, event.touch_count, event.x, event.y, event.value, handle->user_data);
            }
        });
    } else {
        handle->reader->SetEventCallback(nullptr);
    }
}

// Event queue functions
int touch_reader_get_next_event(TouchReaderHandle handle, int* event_type, int* touch_count, int* x, int* y, int* value) {
    if (!handle || !event_type || !touch_count || !x || !y || !value) return -1;
    
    TouchScreen::TouchEvent event;
    if (handle->reader->GetNextEvent(event)) {
        *event_type = static_cast<int>(event.type);
        *touch_count = event.touch_count;
        *x = event.x;
        *y = event.y;
        *value = event.value;
        return 1;
    }
    
    return 0;
}

int touch_reader_wait_for_event(TouchReaderHandle handle, int* event_type, int* touch_count, int* x, int* y, int* value, int timeout_ms) {
    if (!handle || !event_type || !touch_count || !x || !y || !value) return -1;
    TouchScreen::TouchEvent event;
    if (handle->reader->WaitForEvent(event, timeout_ms)) {
        *event_type = static_cast<int>(event.type);
        *touch_count = event.touch_count;
        *x = event.x;
        *y = event.y;
        *value = event.value;
        return 1;
    }
    return 0;
}

void touch_reader_clear_events(TouchReaderHandle handle) {
    if (handle) {
        handle->reader->ClearEvents();
    }
}

// Touch query functions
int touch_reader_get_touch_count(TouchReaderHandle handle) {
    if (!handle) return 0;
    return handle->reader->GetTouchCount();
}

int touch_reader_get_touch_x(TouchReaderHandle handle, int index) {
    if (!handle) return -1;
    int x, y;
    if (handle->reader->GetTouchCoordinates(index, x, y)) {
        return x;
    }
    return -1;
}

int touch_reader_get_touch_y(TouchReaderHandle handle, int index) {
    if (!handle) return -1;
    int x, y;
    if (handle->reader->GetTouchCoordinates(index, x, y)) {
        return y;
    }
    return -1;
}

int touch_reader_get_touch_raw_x(TouchReaderHandle handle, int index) {
    if (!handle) return -1;
    // Instead of returning raw coordinates, return calibrated ones
    int x, y;
    if (handle->reader->GetTouchCoordinates(index, x, y)) {
        return x;
    }
    return -1;
}

int touch_reader_get_touch_raw_y(TouchReaderHandle handle, int index) {
    if (!handle) return -1;
    // Instead of returning raw coordinates, return calibrated ones
    int x, y;
    if (handle->reader->GetTouchCoordinates(index, x, y)) {
        return y;
    }
    return -1;
}

// Calibration functions
void touch_reader_set_calibration(TouchReaderHandle handle, int min_x, int max_x, int min_y, int max_y, int screen_width, int screen_height) {
    if (handle) {
        handle->reader->SetCalibration(min_x, max_x, min_y, max_y, screen_width, screen_height);
    }
}

void touch_reader_set_calibration_margin(TouchReaderHandle handle, double margin_percent) {
    if (handle) {
        handle->reader->SetCalibrationMargin(margin_percent);
    }
}

void touch_reader_set_affine_calibration(TouchReaderHandle handle, const double* matrix, int screen_width, int screen_height) {
    if (!handle || !matrix) {
        return;
    }
    std::array<double, 6> affine{};
    for (size_t i = 0; i < affine.size(); ++i) {
        affine[i] = matrix[i];
    }
    handle->reader->SetAffineCalibration(affine, screen_width, screen_height);
}

void touch_reader_set_calibration_offset(TouchReaderHandle handle, int x_offset, int y_offset) {
    if (handle) {
        handle->reader->SetCalibrationOffset(x_offset, y_offset);
    }
}

void touch_reader_get_calibration(TouchReaderHandle handle, int* min_x, int* max_x, int* min_y, int* max_y) {
    if (handle && min_x && max_x && min_y && max_y) {
        auto calibration = handle->reader->GetCalibration();
        *min_x = static_cast<int>(std::lround(calibration.min_x));
        *max_x = static_cast<int>(std::lround(calibration.max_x));
        *min_y = static_cast<int>(std::lround(calibration.min_y));
        *max_y = static_cast<int>(std::lround(calibration.max_y));
    }
}

int touch_reader_load_calibration(TouchReaderHandle handle, const char* filename) {
    if (!handle || !filename) return -1;
    return handle->reader->LoadCalibration(filename) ? 0 : -1;
}

int touch_reader_save_calibration(TouchReaderHandle handle, const char* filename) {
    if (!handle || !filename) return -1;
    return handle->reader->SaveCalibration(filename) ? 0 : -1;
}

int touch_reader_run_calibration(TouchReaderHandle handle, int screen_width, int screen_height) {
    if (!handle || !handle->reader) {
        fprintf(stderr, "Invalid TouchReader handle in touch_reader_run_calibration\n");
        return -1;
    }
    
    // Print debug information
    printf("Running calibration with screen dimensions: %d x %d\n", screen_width, screen_height);
    
    // Make sure the dimensions are reasonable
    if (screen_width <= 0 || screen_height <= 0) {
        fprintf(stderr, "Invalid screen dimensions: %d x %d\n", screen_width, screen_height);
        return -1;
    }
    
    try {
        return handle->reader->RunCalibration(screen_width, screen_height) ? 0 : -1;
    } catch (const std::exception& e) {
        fprintf(stderr, "Exception in RunCalibration: %s\n", e.what());
        return -1;
    } catch (...) {
        fprintf(stderr, "Unknown exception in RunCalibration\n");
        return -1;
    }
}

int touch_reader_run_calibration_with_monitor(TouchReaderHandle handle, int screen_width, int screen_height, int monitor_index) {
    if (!handle || !handle->reader) {
        fprintf(stderr, "Invalid TouchReader handle in touch_reader_run_calibration_with_monitor\n");
        return -1;
    }
    
    // Print debug information
    printf("Running calibration with screen dimensions: %d x %d for monitor %d\n", 
           screen_width, screen_height, monitor_index);
    
    // Make sure the dimensions are reasonable
    if (screen_width <= 0 || screen_height <= 0) {
        fprintf(stderr, "Invalid screen dimensions: %d x %d\n", screen_width, screen_height);
        return -1;
    }

    try {
        // Setup monitor-specific calibration if needed
        bool success = false;
        
        // Apply xinput transformation if requested for a specific monitor
        if (monitor_index >= 0) {
            // We would execute apply_monitor_transform.sh here
            char cmd[256];
            snprintf(cmd, sizeof(cmd), "./apply_monitor_transform.sh --monitor %d", monitor_index);
            printf("Applying monitor transformation: %s\n", cmd);
            int result = system(cmd);
            if (result != 0) {
                fprintf(stderr, "Warning: Failed to apply monitor transformation\n");
            }
            
            // Run calibration
            success = handle->reader->RunCalibration(screen_width, screen_height);
            
            // Save the calibration with monitor-specific filename
            if (success) {
                char filename[256];
                snprintf(filename, sizeof(filename), "touch_calibration_mon%d.ini", monitor_index);
                handle->reader->SaveCalibration(filename);
                printf("Saved calibration to %s\n", filename);
            }
        }
        else {
            // Reset to full desktop first
            if (system("./apply_monitor_transform.sh --reset") != 0) {
                DEBUG_PRINT("Warning: Failed to reset monitor transform");
            }
            
            // Run calibration
            success = handle->reader->RunCalibration(screen_width, screen_height);
            
            // Save the calibration
            if (success) {
                handle->reader->SaveCalibration("touch_calibration.ini");
                printf("Saved calibration to touch_calibration.ini\n");
            }
        }
        
        return success ? 0 : -1;
    } catch (const std::exception& e) {
        fprintf(stderr, "Exception in RunCalibrationWithMonitor: %s\n", e.what());
        return -1;
    } catch (...) {
        fprintf(stderr, "Unknown exception in RunCalibrationWithMonitor\n");
        return -1;
    }
}

const char* touch_reader_get_selected_device(TouchReaderHandle handle) {
    if (!handle) return nullptr;
    static std::string device;
    device = handle->reader->GetSelectedDevice();
    return device.c_str();
}

int touch_reader_enable_mitm(TouchReaderHandle handle, int enable, int grab_source) {
    if (!handle) return -1;
    return handle->reader->EnableMitm(enable != 0, grab_source != 0) ? 0 : -1;
}

} // extern "C" 
