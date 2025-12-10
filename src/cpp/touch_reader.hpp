#ifndef TOUCH_READER_HPP
#define TOUCH_READER_HPP

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <queue>

namespace TouchScreen {

// Event types that can be returned by the library
enum class EventType {
    TouchDown = 0,
    TouchUp,
    TouchMove,
    SwipeLeft,
    SwipeRight,
    SwipeUp,
    SwipeDown,
    PinchIn,
    PinchOut,
    LongPress,
    DoubleTap,
    Rotate
};

// Touch data structure
struct TouchPoint {
    int tracking_id;
    int raw_x;
    int raw_y;
    int x;  // Calibrated coordinates
    int y;
    int start_x;  // For gesture detection
    int start_y;
    int64_t timestamp;
};

// Event data structure
struct TouchEvent {
    EventType type;
    int touch_count;
    int x;  // Primary x coordinate
    int y;  // Primary y coordinate
    int raw_x;
    int raw_y;
    int value;  // Additional value (like pinch distance)
    std::vector<TouchPoint> touches;
    int64_t timestamp;
};

// Calibration structure
struct Calibration {
    int min_x;
    int max_x;
    int min_y;
    int max_y;
    int screen_width;
    int screen_height;
    float x_factor;
    float y_factor;
    int x_offset;
    int y_offset;
};

// Event callback type
using EventCallback = std::function<void(const TouchEvent&)>;

// Forward declaration of implementation class
class TouchReaderImpl;

// Main TouchReader class
class TouchReader {
public:
    // Constructor
    TouchReader();
    
    // Destructor
    ~TouchReader();
    
    // Disable copy and move
    TouchReader(const TouchReader&) = delete;
    TouchReader& operator=(const TouchReader&) = delete;
    TouchReader(TouchReader&&) = delete;
    TouchReader& operator=(TouchReader&&) = delete;
    
    // Start touch reader with specific device
    bool Start(const std::string& device);
    
    // Start with auto-detection
    bool StartAuto();
    
    // Stop touch reader
    void Stop();
    
    // Set event callback
    void SetEventCallback(EventCallback callback);
    
    // Get number of active touches
    int GetTouchCount() const;
    
    // Get touch coordinates
    bool GetTouchCoordinates(int index, int& x, int& y) const;
    
    // Get raw touch coordinates
    bool GetRawTouchCoordinates(int index, int& x, int& y) const;
    
    // Get all active touches
    std::vector<TouchPoint> GetActiveTouches() const;
    
    // Get next event from queue (non-blocking)
    bool GetNextEvent(TouchEvent& event);
    
    // Wait for next event (blocking with timeout)
    bool WaitForEvent(TouchEvent& event, int timeout_ms = -1);
    
    // Clear all pending events
    void ClearEvents();
    
    // Calibration methods
    void SetCalibration(int min_x, int max_x, int min_y, int max_y, int screen_width, int screen_height);
    void SetCalibrationOffset(int x_offset, int y_offset);
    Calibration GetCalibration() const;
    bool LoadCalibration(const std::string& filename);
    bool SaveCalibration(const std::string& filename);
    bool RunCalibration(int screen_width, int screen_height);
    
    // Get selected device
    std::string GetSelectedDevice() const;

    // Enable/disable OS-level injection of calibrated events (via uinput)
    // When enabled, optionally grab the source device to avoid duplicate OS events
    bool EnableMitm(bool enable, bool grab_source = true);

private:
    // Implementation using pimpl idiom
    std::unique_ptr<TouchReaderImpl> impl_;
};

} // namespace TouchScreen

// C-compatible API for interop
extern "C" {
    // Opaque handle type
    struct TouchReaderHandleStruct;
    typedef TouchReaderHandleStruct* TouchReaderHandle;
    
    // Create/destroy functions
    TouchReaderHandle touch_reader_create();
    void touch_reader_destroy(TouchReaderHandle handle);
    
    // Open/close functions
    int touch_reader_open(TouchReaderHandle handle, const char* device);
    void touch_reader_close(TouchReaderHandle handle);

    // Start/stop functions
    int touch_reader_start(TouchReaderHandle handle, const char* device);
    int touch_reader_start_auto(TouchReaderHandle handle);
    void touch_reader_stop(TouchReaderHandle handle);
    
    // Set callback function
    typedef void (*TouchEventCallbackFn)(int event_type, int touch_count, int x, int y, int value, void* user_data);
    void touch_reader_set_callback(TouchReaderHandle handle, TouchEventCallbackFn callback, void* user_data);
    
    // Event queue functions
    int touch_reader_get_next_event(TouchReaderHandle handle, int* event_type, int* touch_count, int* x, int* y, int* value);
     // Blocking wait for next event with timeout (ms). Returns 1 if event returned, 0 on timeout, -1 on error
     int touch_reader_wait_for_event(TouchReaderHandle handle, int* event_type, int* touch_count, int* x, int* y, int* value, int timeout_ms);
    void touch_reader_clear_events(TouchReaderHandle handle);
    
    // Touch query functions
    int touch_reader_get_touch_count(TouchReaderHandle handle);
    int touch_reader_get_touch_x(TouchReaderHandle handle, int index);
    int touch_reader_get_touch_y(TouchReaderHandle handle, int index);
    int touch_reader_get_touch_raw_x(TouchReaderHandle handle, int index);
    int touch_reader_get_touch_raw_y(TouchReaderHandle handle, int index);
    
    // Calibration functions
    void touch_reader_set_calibration(TouchReaderHandle handle, int min_x, int max_x, int min_y, int max_y, int screen_width, int screen_height);
    void touch_reader_set_calibration_offset(TouchReaderHandle handle, int x_offset, int y_offset);
    void touch_reader_get_calibration(TouchReaderHandle handle, int* min_x, int* max_x, int* min_y, int* max_y);
    int touch_reader_load_calibration(TouchReaderHandle handle, const char* filename);
    int touch_reader_save_calibration(TouchReaderHandle handle, const char* filename);
    int touch_reader_run_calibration(TouchReaderHandle handle, int screen_width, int screen_height);
    int touch_reader_run_calibration_with_monitor(TouchReaderHandle handle, int screen_width, int screen_height, int monitor_index);
    
    // Get selected device
    const char* touch_reader_get_selected_device(TouchReaderHandle handle);

     // Enable/disable OS-level injection of calibrated events (uinput). grab_source=1 will EVIOCGRAB the source
     int touch_reader_enable_mitm(TouchReaderHandle handle, int enable, int grab_source);
}

#endif // TOUCH_READER_HPP 