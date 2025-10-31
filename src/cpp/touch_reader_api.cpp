#include "touch_reader.hpp"

// Opaque handle structure
struct TouchReaderHandleStruct {
    TouchScreen::TouchReader* reader;
    TouchEventCallbackFn callback;
    void* user_data;
};

extern "C" {

int touch_reader_open(TouchReaderHandle handle, const char* device) {
    if (!handle || !device) return -1;
    return handle->reader->Start(device) ? 0 : -1;
}

void touch_reader_close(TouchReaderHandle handle) {
    if (handle) {
        handle->reader->Stop();
    }
}

} // extern "C"