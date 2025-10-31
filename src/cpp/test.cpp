#include "touch_reader.hpp"
#include "device_helper.hpp"
#include "ini_parser.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using TouchScreen::TouchReader;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

std::atomic<bool> g_running{true};

void signalHandler(int) {
    g_running = false;
}

std::string trim(const std::string& value) {
    auto begin = value.begin();
    while (begin != value.end() && std::isspace(static_cast<unsigned char>(*begin))) {
        ++begin;
    }
    auto end = value.end();
    while (end != begin && std::isspace(static_cast<unsigned char>(*(end - 1)))) {
        --end;
    }
    return std::string(begin, end);
}

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string slugify(const std::string& input) {
    std::string slug;
    slug.reserve(input.size());
    char previous = '\0';
    for (unsigned char c : input) {
        if (std::isalnum(c)) {
            slug.push_back(static_cast<char>(std::tolower(c)));
            previous = slug.back();
        } else if (c == ' ' || c == '-' || c == '_' || c == '.') {
            if (!slug.empty() && previous != '_') {
                slug.push_back('_');
                previous = '_';
            }
        }
    }
    while (!slug.empty() && slug.back() == '_') {
        slug.pop_back();
    }
    if (slug.empty()) {
        slug = "device";
    }
    return slug;
}

std::string runCommand(const std::string& command) {
    std::array<char, 4096> buffer{};
    std::string result;
    std::unique_ptr<FILE, int(*)(FILE*)> pipe(popen(command.c_str(), "r"), pclose);
    if (!pipe) {
        return result;
    }
    while (fgets(buffer.data(), buffer.size(), pipe.get())) {
        result.append(buffer.data());
    }
    return result;
}

struct MonitorInfo {
    int index = -1;
    std::string name;
    bool primary = false;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    double scale_x = 1.0;
    double scale_y = 1.0;
    std::string rotation = "normal";
    std::string edid_hash;
};

struct DesktopLayout {
    std::vector<MonitorInfo> monitors;
    int origin_x = 0;
    int origin_y = 0;
    int width = 0;
    int height = 0;
    std::string hash;
};

uint64_t fnv1a(const std::string& data) {
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : data) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string hashString(const std::string& data) {
    std::ostringstream oss;
    oss << std::hex << std::nouppercase << fnv1a(data);
    return oss.str();
}

DesktopLayout detectDesktopLayout(std::string& error) {
    DesktopLayout layout;
    std::string output = runCommand("xrandr --listmonitors");
    if (output.empty()) {
        error = "Failed to invoke xrandr --listmonitors";
        return layout;
    }

    std::istringstream iss(output);
    std::string line;
    // Skip header
    std::getline(iss, line);

    std::vector<std::string> geometryTokens;
    while (std::getline(iss, line)) {
        auto trimmed = trim(line);
        if (trimmed.empty()) {
            continue;
        }
        std::istringstream ls(trimmed);
        std::vector<std::string> tokens;
        std::string token;
        while (ls >> token) {
            tokens.push_back(token);
        }
        if (tokens.size() < 3) {
            continue;
        }
        MonitorInfo monitor;
        std::string idxToken = tokens[0];
        if (auto pos = idxToken.find(':'); pos != std::string::npos) {
            idxToken = idxToken.substr(0, pos);
        }
        try {
            monitor.index = std::stoi(idxToken);
        } catch (...) {
            monitor.index = static_cast<int>(layout.monitors.size());
        }

        std::string indicator = tokens[1];
        monitor.primary = indicator.find('*') != std::string::npos;

        // Determine geometry token (contains 'x' and '+')
        std::string geometry;
        for (const auto& candidate : tokens) {
            if (candidate.find('x') != std::string::npos && candidate.find('+') != std::string::npos) {
                geometry = candidate;
                break;
            }
        }
        if (geometry.empty()) {
            continue;
        }

        std::regex geomRegex(R"((\d+)/\d+x(\d+)/\d+([+-]\d+)([+-]\d+))");
        std::smatch match;
        if (!std::regex_search(geometry, match, geomRegex) || match.size() < 5) {
            continue;
        }
        monitor.width = std::stoi(match[1].str());
        monitor.height = std::stoi(match[2].str());
        monitor.x = std::stoi(match[3].str());
        monitor.y = std::stoi(match[4].str());

        // Monitor name is last token
        monitor.name = tokens.back();
        layout.monitors.push_back(monitor);
    }

    if (layout.monitors.empty()) {
        error = "No active monitors detected";
        return layout;
    }

    // Parse verbose data for rotation, scale, EDID
    std::string verbose = runCommand("xrandr --verbose");
    std::istringstream vs(verbose);
    std::string currentLine;
    MonitorInfo* current = nullptr;
    while (std::getline(vs, currentLine)) {
        if (currentLine.empty()) {
            continue;
        }
        if (!std::isspace(static_cast<unsigned char>(currentLine.front()))) {
            auto trimmed = trim(currentLine);
            current = nullptr;
            for (auto& monitor : layout.monitors) {
                if (trimmed.rfind(monitor.name + " ", 0) == 0 || trimmed == monitor.name) {
                    current = &monitor;
                    if (auto pos = trimmed.find('('); pos != std::string::npos) {
                        auto end = trimmed.find(')', pos + 1);
                        if (end != std::string::npos && end > pos + 1) {
                            std::istringstream rot(trimmed.substr(pos + 1, end - pos - 1));
                            std::string rotationToken;
                            rot >> rotationToken;
                            if (!rotationToken.empty()) {
                                current->rotation = toLower(rotationToken);
                            }
                        }
                    }
                    break;
                }
            }
            continue;
        }
        if (!current) {
            continue;
        }
        auto trimmed = trim(currentLine);
        if (trimmed.rfind("Scale:", 0) == 0) {
            std::istringstream ss(trimmed.substr(6));
            double sx = 1.0;
            double sy = 1.0;
            char xchar;
            ss >> sx >> xchar >> sy;
            if (sx > 0.0) current->scale_x = sx;
            if (sy > 0.0) current->scale_y = sy;
        } else if (trimmed == "EDID:") {
            std::string edidData;
            std::streampos pos = vs.tellg();
            std::string edidLine;
            while (std::getline(vs, edidLine)) {
                if (edidLine.empty() || !std::isspace(static_cast<unsigned char>(edidLine.front()))) {
                    vs.seekg(pos);
                    break;
                }
                pos = vs.tellg();
                edidData += trim(edidLine);
            }
            if (!edidData.empty()) {
                edidData.erase(std::remove_if(edidData.begin(), edidData.end(), ::isspace), edidData.end());
                current->edid_hash = hashString(edidData);
            }
        }
    }

    int min_x = layout.monitors.front().x;
    int min_y = layout.monitors.front().y;
    int max_x = layout.monitors.front().x + layout.monitors.front().width;
    int max_y = layout.monitors.front().y + layout.monitors.front().height;

    for (const auto& monitor : layout.monitors) {
        min_x = std::min(min_x, monitor.x);
        min_y = std::min(min_y, monitor.y);
        max_x = std::max(max_x, monitor.x + monitor.width);
        max_y = std::max(max_y, monitor.y + monitor.height);
    }

    layout.origin_x = min_x;
    layout.origin_y = min_y;
    layout.width = max_x - min_x;
    layout.height = max_y - min_y;

    std::ostringstream hashStream;
    hashStream << layout.origin_x << ',' << layout.origin_y << ',' << layout.width << ',' << layout.height << ';';
    for (const auto& monitor : layout.monitors) {
        hashStream << monitor.name << '|' << monitor.x << '|' << monitor.y << '|' << monitor.width << '|' << monitor.height << '|' << monitor.rotation << '|' << monitor.scale_x << '|' << monitor.scale_y << '|' << monitor.edid_hash << ';';
    }
    layout.hash = hashString(hashStream.str());
    return layout;
}

std::optional<MonitorInfo> findMonitorByIndex(const DesktopLayout& layout, int index) {
    for (const auto& monitor : layout.monitors) {
        if (monitor.index == index) {
            return monitor;
        }
    }
    if (!layout.monitors.empty() && index >= 0 && index < static_cast<int>(layout.monitors.size())) {
        return layout.monitors[index];
    }
    return std::nullopt;
}

std::optional<MonitorInfo> findMonitorByName(const DesktopLayout& layout, const std::string& name) {
    std::string lowered = toLower(name);
    for (const auto& monitor : layout.monitors) {
        if (toLower(monitor.name) == lowered) {
            return monitor;
        }
    }
    return std::nullopt;
}

std::array<double, 9> identityMatrix() {
    return {1.0, 0.0, 0.0,
            0.0, 1.0, 0.0,
            0.0, 0.0, 1.0};
}

std::array<double, 9> computeCtm(const DesktopLayout& layout, const MonitorInfo& monitor) {
    double desktop_width = static_cast<double>(layout.width);
    double desktop_height = static_cast<double>(layout.height);
    if (desktop_width <= 0.0) desktop_width = 1.0;
    if (desktop_height <= 0.0) desktop_height = 1.0;

    double offset_x = static_cast<double>(monitor.x - layout.origin_x);
    double offset_y = static_cast<double>(monitor.y - layout.origin_y);
    double width = static_cast<double>(monitor.width);
    double height = static_cast<double>(monitor.height);

    if (monitor.scale_x > 0.0) {
        width *= monitor.scale_x;
        offset_x *= monitor.scale_x;
    }
    if (monitor.scale_y > 0.0) {
        height *= monitor.scale_y;
        offset_y *= monitor.scale_y;
    }

    double m0 = 1.0, m1 = 0.0, m2 = 0.0;
    double m3 = 0.0, m4 = 1.0, m5 = 0.0;

    std::string rotation = toLower(monitor.rotation);

    if (rotation == "normal" || rotation.empty()) {
        m0 = width;
        m1 = 0.0;
        m2 = offset_x;
        m3 = 0.0;
        m4 = height;
        m5 = offset_y;
    } else if (rotation == "inverted") {
        m0 = -width;
        m1 = 0.0;
        m2 = offset_x + width;
        m3 = 0.0;
        m4 = -height;
        m5 = offset_y + height;
    } else if (rotation == "left") {
        m0 = 0.0;
        m1 = height;
        m2 = offset_x;
        m3 = -width;
        m4 = 0.0;
        m5 = offset_y + width;
    } else if (rotation == "right") {
        m0 = 0.0;
        m1 = height;
        m2 = offset_x;
        m3 = width;
        m4 = 0.0;
        m5 = offset_y;
        // adjust for vertical orientation
        m1 = -height;
        m2 = offset_x + height;
        m3 = width;
        m4 = 0.0;
        m5 = offset_y;
    } else {
        m0 = width;
        m1 = 0.0;
        m2 = offset_x;
        m3 = 0.0;
        m4 = height;
        m5 = offset_y;
    }

    std::array<double, 9> matrix{
        m0 / desktop_width, m1 / desktop_width, m2 / desktop_width,
        m3 / desktop_height, m4 / desktop_height, m5 / desktop_height,
        0.0, 0.0, 1.0
    };
    return matrix;
}

bool applyCtmToDevice(int deviceId, const std::array<double, 9>& matrix, std::string& error) {
    std::ostringstream cmd;
    cmd << "xinput set-prop " << deviceId << " \"Coordinate Transformation Matrix\"";
    cmd << std::fixed << std::setprecision(6);
    for (double value : matrix) {
        cmd << ' ' << value;
    }
    int rc = std::system(cmd.str().c_str());
    if (rc != 0) {
        error = "xinput set-prop failed for device " + std::to_string(deviceId);
        return false;
    }
    return true;
}

bool applyCtmToDevices(const std::vector<int>& deviceIds, const std::array<double, 9>& matrix, std::string& error) {
    bool ok = true;
    for (int id : deviceIds) {
        if (!applyCtmToDevice(id, matrix, error)) {
            ok = false;
        }
    }
    return ok;
}

std::optional<std::array<double, 9>> readCtmForDevice(int deviceId) {
    std::ostringstream cmd;
    cmd << "xinput list-props " << deviceId;
    std::string output = runCommand(cmd.str());
    if (output.empty()) {
        return std::nullopt;
    }
    std::istringstream iss(output);
    std::string line;
    std::array<double, 9> matrix{};
    bool found = false;
    while (std::getline(iss, line)) {
        if (line.find("Coordinate Transformation Matrix") != std::string::npos) {
            std::string valuesLine = line.substr(line.find(':') + 1);
            std::istringstream vs(valuesLine);
            for (size_t i = 0; i < 9; ++i) {
                if (!(vs >> matrix[i])) {
                    matrix[i] = 0.0;
                }
            }
            found = true;
            break;
        }
    }
    if (!found) {
        return std::nullopt;
    }
    return matrix;
}

void printMatrix(const std::array<double, 9>& matrix) {
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "[" << matrix[0] << ' ' << matrix[1] << ' ' << matrix[2] << "]\n";
    std::cout << " " << matrix[3] << ' ' << matrix[4] << ' ' << matrix[5] << "\n";
    std::cout << " " << matrix[6] << ' ' << matrix[7] << ' ' << matrix[8] << "\n";
}

struct DeviceContext {
    int id = -1;
    std::string name;
    std::string path;
    std::vector<int> related_ids;
    std::unordered_map<int, std::string> id_to_name;
    TouchScreen::DeviceHelper::DeviceInfo ranges;
};

struct Options {
    bool show_help = false;
    bool calibrate = false;
    bool load_calibration = false;
    bool list_devices = false;
    bool list_monitors = false;
    bool status = false;
    bool save_profile = false;
    bool load_profile = false;
    bool list_profiles = false;
    bool reapply = false;
    bool include_related_tools = true;
    bool use_affine = false;
    bool reset_mapping = false;
    bool map_full_desktop = false;
    bool run_event_loop = true;
    bool show_udev_instructions = false;
    double margin_percent = 0.5;
    int device_id = -1;
    std::string device_path;
    std::string monitor_name;
    int monitor_index = -1;
    int screen_width = 0;
    int screen_height = 0;
    std::string config_dir;
    std::string calibration_dir;
    std::string profile_dir;
    std::string profile_to_save;
    std::string profile_to_load;
    std::unordered_set<std::string> tool_filters;
};

Options parseArguments(int argc, char* argv[], std::string& error) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            options.show_help = true;
        } else if (arg == "-c" || arg == "--calibrate") {
            options.calibrate = true;
        } else if (arg == "-l" || arg == "--load") {
            options.load_calibration = true;
        } else if (arg == "--list-devices") {
            options.list_devices = true;
        } else if (arg == "--list-monitors") {
            options.list_monitors = true;
        } else if (arg == "--status") {
            options.status = true;
            options.run_event_loop = false;
        } else if (arg == "--save-profile") {
            if (i + 1 >= argc) { error = "--save-profile requires a name"; break; }
            options.save_profile = true;
            options.profile_to_save = argv[++i];
        } else if (arg == "--load-profile") {
            if (i + 1 >= argc) { error = "--load-profile requires a name"; break; }
            options.load_profile = true;
            options.profile_to_load = argv[++i];
        } else if (arg == "--list-profiles") {
            options.list_profiles = true;
        } else if (arg == "--reapply") {
            options.reapply = true;
        } else if (arg == "--no-loop") {
            options.run_event_loop = false;
        } else if (arg == "--reset-ctm" || arg == "--reset-mapping") {
            options.reset_mapping = true;
        } else if (arg == "--map-full") {
            options.map_full_desktop = true;
            options.monitor_index = -1;
        } else if (arg == "--device" || arg == "-d") {
            if (i + 1 >= argc) { error = "--device requires a path"; break; }
            options.device_path = argv[++i];
        } else if (arg == "--device-id") {
            if (i + 1 >= argc) { error = "--device-id requires a numeric value"; break; }
            options.device_id = std::stoi(argv[++i]);
        } else if (arg == "--monitor" || arg == "-m") {
            if (i + 1 >= argc) { error = "--monitor requires an index"; break; }
            options.monitor_index = std::stoi(argv[++i]);
        } else if (arg == "--monitor-name") {
            if (i + 1 >= argc) { error = "--monitor-name requires a value"; break; }
            options.monitor_name = argv[++i];
        } else if (arg == "--resolution" || arg == "-r") {
            if (i + 1 >= argc) { error = "--resolution requires WIDTHxHEIGHT"; break; }
            std::string res = argv[++i];
            auto pos = res.find('x');
            if (pos == std::string::npos) {
                error = "Invalid resolution format";
                break;
            }
            options.screen_width = std::stoi(res.substr(0, pos));
            options.screen_height = std::stoi(res.substr(pos + 1));
        } else if (arg == "--margin") {
            if (i + 1 >= argc) { error = "--margin requires a percentage"; break; }
            options.margin_percent = std::stod(argv[++i]);
        } else if (arg == "--affine") {
            options.use_affine = true;
        } else if (arg == "--config-dir") {
            if (i + 1 >= argc) { error = "--config-dir requires a path"; break; }
            options.config_dir = argv[++i];
        } else if (arg == "--calibration-dir") {
            if (i + 1 >= argc) { error = "--calibration-dir requires a path"; break; }
            options.calibration_dir = argv[++i];
        } else if (arg == "--profile-dir") {
            if (i + 1 >= argc) { error = "--profile-dir requires a path"; break; }
            options.profile_dir = argv[++i];
        } else if (arg == "--tool") {
            if (i + 1 >= argc) { error = "--tool requires a value"; break; }
            std::string tools = argv[++i];
            std::stringstream ss(tools);
            std::string item;
            while (std::getline(ss, item, ',')) {
                auto lowered = toLower(trim(item));
                if (!lowered.empty()) {
                    options.tool_filters.insert(lowered);
                }
            }
        } else if (arg == "--no-related-tools") {
            options.include_related_tools = false;
        } else if (arg == "--udev-install" || arg == "--udev-rule") {
            options.show_udev_instructions = true;
        } else {
            error = "Unknown argument: " + arg;
            break;
        }
    }
    return options;
}

struct CalibrationPoint {
    double raw_x = 0.0;
    double raw_y = 0.0;
    double target_x = 0.0;
    double target_y = 0.0;
};

bool gaussianSolve3x3(double matrix[3][3], double vector[3], double out[3]) {
    int perm[3] = {0, 1, 2};
    for (int i = 0; i < 3; ++i) {
        double pivot = std::fabs(matrix[i][i]);
        int pivotRow = i;
        for (int r = i + 1; r < 3; ++r) {
            if (std::fabs(matrix[r][i]) > pivot) {
                pivot = std::fabs(matrix[r][i]);
                pivotRow = r;
            }
        }
        if (pivot < 1e-9) {
            return false;
        }
        if (pivotRow != i) {
            for (int c = 0; c < 3; ++c) {
                std::swap(matrix[i][c], matrix[pivotRow][c]);
            }
            std::swap(vector[i], vector[pivotRow]);
        }
        double diag = matrix[i][i];
        for (int c = i; c < 3; ++c) {
            matrix[i][c] /= diag;
        }
        vector[i] /= diag;
        for (int r = 0; r < 3; ++r) {
            if (r == i) continue;
            double factor = matrix[r][i];
            for (int c = i; c < 3; ++c) {
                matrix[r][c] -= factor * matrix[i][c];
            }
            vector[r] -= factor * vector[i];
        }
    }
    out[0] = vector[0];
    out[1] = vector[1];
    out[2] = vector[2];
    return true;
}

bool solveLeastSquares(const std::array<std::pair<double, double>, 4>& raw,
                       const std::array<std::pair<double, double>, 4>& target,
                       std::array<double, 6>& matrix) {
    double MX[3][3] = {{0.0}};
    double MY[3][3] = {{0.0}};
    double bx[3] = {0.0};
    double by[3] = {0.0};

    for (size_t i = 0; i < raw.size(); ++i) {
        double rx = raw[i].first;
        double ry = raw[i].second;
        double tx = target[i].first;
        double ty = target[i].second;
        double v[3] = {rx, ry, 1.0};
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                MX[r][c] += v[r] * v[c];
                MY[r][c] += v[r] * v[c];
            }
        }
        for (int r = 0; r < 3; ++r) {
            bx[r] += v[r] * tx;
            by[r] += v[r] * ty;
        }
    }

    double ax[3];
    double ay[3];
    if (!gaussianSolve3x3(MX, bx, ax)) {
        return false;
    }
    if (!gaussianSolve3x3(MY, by, ay)) {
        return false;
    }
    matrix = {ax[0], ax[1], ax[2], ay[0], ay[1], ay[2]};
    return true;
}

struct CalibrationResult {
    double min_x = 0.0;
    double max_x = 0.0;
    double min_y = 0.0;
    double max_y = 0.0;
    std::array<double, 6> affine{1.0, 0.0, 0.0, 0.0, 1.0, 0.0};
};

bool runCalibration(TouchReader& reader,
                    int screen_width,
                    int screen_height,
                    double margin_percent,
                    bool use_affine,
                    CalibrationResult& result) {
    std::atomic<int> step{0};
    std::array<std::pair<double, double>, 4> raw_points{};
    std::array<std::pair<double, double>, 4> target_points{
        std::make_pair(20.0, 20.0),
        std::make_pair(static_cast<double>(screen_width) - 20.0, 20.0),
        std::make_pair(static_cast<double>(screen_width) - 20.0, static_cast<double>(screen_height) - 20.0),
        std::make_pair(20.0, static_cast<double>(screen_height) - 20.0)
    };

    std::mutex capture_mutex;
    std::condition_variable capture_cv;
    bool captured = false;

    reader.SetEventCallback([&](const TouchScreen::TouchEvent& event) {
        if (event.type == TouchScreen::EventType::TouchDown && event.touch_count > 0) {
            int current = step.load();
            if (current >= 0 && current < 4) {
                std::lock_guard<std::mutex> lock(capture_mutex);
                raw_points[current] = {event.touches[0].raw_x, event.touches[0].raw_y};
                captured = true;
                capture_cv.notify_all();
            }
        }
    });

    const char* labels[4] = {"top-left", "top-right", "bottom-right", "bottom-left"};
    for (int i = 0; i < 4; ++i) {
        step = i;
        {
            std::lock_guard<std::mutex> lock(capture_mutex);
            captured = false;
        }
        std::cout << "Touch the " << labels[i] << " corner..." << std::endl;
        std::unique_lock<std::mutex> lock(capture_mutex);
        if (!capture_cv.wait_for(lock, 15s, [&] { return captured; })) {
            std::cerr << "Timeout waiting for point " << (i + 1) << std::endl;
            return false;
        }
        std::cout << "Captured raw point: (" << raw_points[i].first << ", " << raw_points[i].second << ")" << std::endl;
    }

    double min_x = raw_points[0].first;
    double max_x = raw_points[0].first;
    double min_y = raw_points[0].second;
    double max_y = raw_points[0].second;
    for (const auto& point : raw_points) {
        min_x = std::min(min_x, point.first);
        max_x = std::max(max_x, point.first);
        min_y = std::min(min_y, point.second);
        max_y = std::max(max_y, point.second);
    }

    double range_x = max_x - min_x;
    double range_y = max_y - min_y;
    if (range_x <= 0.0 || range_y <= 0.0) {
        std::cerr << "Invalid calibration data captured." << std::endl;
        return false;
    }

    if (margin_percent > 0.0) {
        double adjust_x = range_x * (margin_percent / 100.0);
        double adjust_y = range_y * (margin_percent / 100.0);
        min_x += adjust_x;
        max_x -= adjust_x;
        min_y += adjust_y;
        max_y -= adjust_y;
    }

    result.min_x = min_x;
    result.max_x = max_x;
    result.min_y = min_y;
    result.max_y = max_y;

    if (use_affine) {
        if (!solveLeastSquares(raw_points, target_points, result.affine)) {
            std::cerr << "Failed to compute affine calibration; falling back to min/max." << std::endl;
        }
    }

    return true;
}

std::vector<int> filterDeviceIdsByTool(const std::vector<int>& ids,
                                       const std::unordered_map<int, std::string>& names,
                                       const std::unordered_set<std::string>& filters) {
    if (filters.empty()) {
        return ids;
    }
    std::vector<int> filtered;
    for (int id : ids) {
        auto it = names.find(id);
        if (it == names.end()) {
            continue;
        }
        auto name = toLower(it->second);
        bool include = false;
        for (const auto& filter : filters) {
            if (name.find(filter) != std::string::npos) {
                include = true;
                break;
            }
        }
        if (include) {
            filtered.push_back(id);
        }
    }
    if (filtered.empty()) {
        return ids;
    }
    return filtered;
}

void listDevices() {
    auto devices = TouchScreen::DeviceHelper::enumerateDevices();
    if (devices.empty()) {
        std::cout << "No devices found via xinput." << std::endl;
        return;
    }
    std::cout << "Available input devices:" << std::endl;
    for (const auto& device : devices) {
        std::cout << "  ID " << device.id << ": " << device.name;
        if (device.is_absolute) {
            std::cout << " [absolute " << device.max_x << "x" << device.max_y << "]";
        } else {
            std::cout << " [relative]";
        }
        if (!device.path.empty()) {
            std::cout << " -- " << device.path;
        }
        std::cout << std::endl;
    }
}

void listMonitors(const DesktopLayout& layout) {
    std::cout << "Detected monitors (layout hash: " << layout.hash << ")" << std::endl;
    for (const auto& monitor : layout.monitors) {
        std::cout << "  [" << monitor.index << "] " << monitor.name << " "
                  << monitor.width << "x" << monitor.height << " +" << monitor.x << "+" << monitor.y;
        if (monitor.primary) {
            std::cout << " (primary)";
        }
        std::cout << " rot=" << monitor.rotation;
        if (monitor.scale_x != 1.0 || monitor.scale_y != 1.0) {
            std::cout << " scale=" << monitor.scale_x << "x" << monitor.scale_y;
        }
        if (!monitor.edid_hash.empty()) {
            std::cout << " edid=" << monitor.edid_hash;
        }
        std::cout << std::endl;
    }
}

void listProfiles(const fs::path& profile_dir) {
    if (!fs::exists(profile_dir)) {
        std::cout << "Profile directory does not exist: " << profile_dir << std::endl;
        return;
    }
    bool any = false;
    for (const auto& entry : fs::directory_iterator(profile_dir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        if (entry.path().extension() != ".ini") {
            continue;
        }
        TouchScreen::Config::IniData data;
        if (!TouchScreen::Config::LoadIni(entry.path().string(), data)) {
            continue;
        }
        any = true;
        auto name = entry.path().stem().string();
        auto monitorName = TouchScreen::Config::GetValue(data, "Profile", "monitor_name").value_or("?");
        auto layoutHash = TouchScreen::Config::GetValue(data, "Profile", "layout_hash").value_or("?");
        std::cout << "  " << name << " -> monitor " << monitorName << ", layout " << layoutHash << std::endl;
    }
    if (!any) {
        std::cout << "No profile files found in " << profile_dir << std::endl;
    }
}

void printUdevInstructions() {
    std::cout << "To grant access to touch devices without root permissions, add a udev rule:" << std::endl;
    std::cout << "  sudo tee /etc/udev/rules.d/99-touchscreen.rules <<'EOF'" << std::endl;
    std::cout << "  SUBSYSTEM==\"input\", GROUP=\"input\", MODE=\"0660\"" << std::endl;
    std::cout << "EOF" << std::endl;
    std::cout << "Then add your user to the 'input' group and reload rules:" << std::endl;
    std::cout << "  sudo usermod -aG input $USER" << std::endl;
    std::cout << "  sudo udevadm control --reload && sudo udevadm trigger" << std::endl;
    std::cout << "Log out and back in to apply the new group membership." << std::endl;
}

struct ProfileData {
    std::string name;
    std::string device_name;
    int device_id = -1;
    bool include_related = true;
    std::unordered_set<std::string> tool_filters;
    std::string layout_hash;
    MonitorInfo monitor;
    DesktopLayout layout_snapshot;
    std::array<double, 9> ctm = identityMatrix();
};

bool saveProfile(const fs::path& path, const ProfileData& profile, std::string& error) {
    TouchScreen::Config::IniData data;
    TouchScreen::Config::SetValue(data, "Profile", "device_id", std::to_string(profile.device_id));
    TouchScreen::Config::SetValue(data, "Profile", "device_name", profile.device_name);
    TouchScreen::Config::SetValue(data, "Profile", "layout_hash", profile.layout_hash);
    TouchScreen::Config::SetValue(data, "Profile", "monitor_name", profile.monitor.name);
    TouchScreen::Config::SetValue(data, "Profile", "monitor_index", std::to_string(profile.monitor.index));
    TouchScreen::Config::SetValue(data, "Profile", "monitor_x", std::to_string(profile.monitor.x));
    TouchScreen::Config::SetValue(data, "Profile", "monitor_y", std::to_string(profile.monitor.y));
    TouchScreen::Config::SetValue(data, "Profile", "monitor_width", std::to_string(profile.monitor.width));
    TouchScreen::Config::SetValue(data, "Profile", "monitor_height", std::to_string(profile.monitor.height));
    TouchScreen::Config::SetValue(data, "Profile", "monitor_rotation", profile.monitor.rotation);
    TouchScreen::Config::SetValue(data, "Profile", "monitor_scale_x", std::to_string(profile.monitor.scale_x));
    TouchScreen::Config::SetValue(data, "Profile", "monitor_scale_y", std::to_string(profile.monitor.scale_y));
    TouchScreen::Config::SetValue(data, "Profile", "include_related", profile.include_related ? "1" : "0");
    if (!profile.tool_filters.empty()) {
        std::ostringstream oss;
        bool first = true;
        for (const auto& filter : profile.tool_filters) {
            if (!first) oss << ',';
            oss << filter;
            first = false;
        }
        TouchScreen::Config::SetValue(data, "Profile", "tool_filters", oss.str());
    }
    TouchScreen::Config::SetValue(data, "Layout", "origin_x", std::to_string(profile.layout_snapshot.origin_x));
    TouchScreen::Config::SetValue(data, "Layout", "origin_y", std::to_string(profile.layout_snapshot.origin_y));
    TouchScreen::Config::SetValue(data, "Layout", "width", std::to_string(profile.layout_snapshot.width));
    TouchScreen::Config::SetValue(data, "Layout", "height", std::to_string(profile.layout_snapshot.height));
    TouchScreen::Config::SetValue(data, "CTM", "m0", std::to_string(profile.ctm[0]));
    TouchScreen::Config::SetValue(data, "CTM", "m1", std::to_string(profile.ctm[1]));
    TouchScreen::Config::SetValue(data, "CTM", "m2", std::to_string(profile.ctm[2]));
    TouchScreen::Config::SetValue(data, "CTM", "m3", std::to_string(profile.ctm[3]));
    TouchScreen::Config::SetValue(data, "CTM", "m4", std::to_string(profile.ctm[4]));
    TouchScreen::Config::SetValue(data, "CTM", "m5", std::to_string(profile.ctm[5]));
    TouchScreen::Config::SetValue(data, "CTM", "m6", std::to_string(profile.ctm[6]));
    TouchScreen::Config::SetValue(data, "CTM", "m7", std::to_string(profile.ctm[7]));
    TouchScreen::Config::SetValue(data, "CTM", "m8", std::to_string(profile.ctm[8]));
    if (!TouchScreen::Config::SaveIni(path.string(), data)) {
        error = "Failed to write profile file";
        return false;
    }
    return true;
}

bool loadProfile(const fs::path& path, ProfileData& profile, std::string& error) {
    TouchScreen::Config::IniData data;
    if (!TouchScreen::Config::LoadIni(path.string(), data)) {
        error = "Failed to load profile file";
        return false;
    }
    profile.name = path.stem().string();
    profile.device_name = TouchScreen::Config::GetValue(data, "Profile", "device_name").value_or("");
    profile.device_id = std::stoi(TouchScreen::Config::GetValue(data, "Profile", "device_id").value_or("-1"));
    profile.layout_hash = TouchScreen::Config::GetValue(data, "Profile", "layout_hash").value_or("");
    profile.monitor.name = TouchScreen::Config::GetValue(data, "Profile", "monitor_name").value_or("");
    profile.monitor.index = std::stoi(TouchScreen::Config::GetValue(data, "Profile", "monitor_index").value_or("-1"));
    profile.monitor.x = std::stoi(TouchScreen::Config::GetValue(data, "Profile", "monitor_x").value_or("0"));
    profile.monitor.y = std::stoi(TouchScreen::Config::GetValue(data, "Profile", "monitor_y").value_or("0"));
    profile.monitor.width = std::stoi(TouchScreen::Config::GetValue(data, "Profile", "monitor_width").value_or("0"));
    profile.monitor.height = std::stoi(TouchScreen::Config::GetValue(data, "Profile", "monitor_height").value_or("0"));
    profile.monitor.rotation = TouchScreen::Config::GetValue(data, "Profile", "monitor_rotation").value_or("normal");
    profile.monitor.scale_x = std::stod(TouchScreen::Config::GetValue(data, "Profile", "monitor_scale_x").value_or("1"));
    profile.monitor.scale_y = std::stod(TouchScreen::Config::GetValue(data, "Profile", "monitor_scale_y").value_or("1"));
    profile.include_related = TouchScreen::Config::GetValue(data, "Profile", "include_related").value_or("1") != "0";
    profile.tool_filters.clear();
    if (auto filters = TouchScreen::Config::GetValue(data, "Profile", "tool_filters")) {
        std::stringstream ss(*filters);
        std::string item;
        while (std::getline(ss, item, ',')) {
            auto lowered = toLower(trim(item));
            if (!lowered.empty()) {
                profile.tool_filters.insert(lowered);
            }
        }
    }
    profile.layout_snapshot.origin_x = std::stoi(TouchScreen::Config::GetValue(data, "Layout", "origin_x").value_or("0"));
    profile.layout_snapshot.origin_y = std::stoi(TouchScreen::Config::GetValue(data, "Layout", "origin_y").value_or("0"));
    profile.layout_snapshot.width = std::stoi(TouchScreen::Config::GetValue(data, "Layout", "width").value_or("0"));
    profile.layout_snapshot.height = std::stoi(TouchScreen::Config::GetValue(data, "Layout", "height").value_or("0"));
    profile.ctm = identityMatrix();
    profile.ctm[0] = std::stod(TouchScreen::Config::GetValue(data, "CTM", "m0").value_or("1"));
    profile.ctm[1] = std::stod(TouchScreen::Config::GetValue(data, "CTM", "m1").value_or("0"));
    profile.ctm[2] = std::stod(TouchScreen::Config::GetValue(data, "CTM", "m2").value_or("0"));
    profile.ctm[3] = std::stod(TouchScreen::Config::GetValue(data, "CTM", "m3").value_or("0"));
    profile.ctm[4] = std::stod(TouchScreen::Config::GetValue(data, "CTM", "m4").value_or("1"));
    profile.ctm[5] = std::stod(TouchScreen::Config::GetValue(data, "CTM", "m5").value_or("0"));
    profile.ctm[6] = std::stod(TouchScreen::Config::GetValue(data, "CTM", "m6").value_or("0"));
    profile.ctm[7] = std::stod(TouchScreen::Config::GetValue(data, "CTM", "m7").value_or("0"));
    profile.ctm[8] = std::stod(TouchScreen::Config::GetValue(data, "CTM", "m8").value_or("1"));
    return true;
}

const char* calibrationModeToString(TouchScreen::CalibrationMode mode) {
    switch (mode) {
        case TouchScreen::CalibrationMode::MinMax: return "minmax";
        case TouchScreen::CalibrationMode::Affine: return "affine";
        default: return "none";
    }
}

void printStatus(const DesktopLayout& layout,
                 const DeviceContext& device,
                 const fs::path& calibration_path,
                 const fs::path& profile_dir,
                 const TouchScreen::Calibration& calibration) {
    std::cout << "=== Status ===" << std::endl;
    std::cout << "Layout hash: " << layout.hash << std::endl;
    listMonitors(layout);

    std::cout << "Device ID: " << device.id << std::endl;
    std::cout << "Device name: " << device.name << std::endl;
    std::cout << "Device path: " << device.path << std::endl;

    if (device.id >= 0) {
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "Calibration mode: " << calibrationModeToString(calibration.mode) << std::endl;
        std::cout << "Calibration X range: [" << calibration.min_x << ", " << calibration.max_x << "]" << std::endl;
        std::cout << "Calibration Y range: [" << calibration.min_y << ", " << calibration.max_y << "]" << std::endl;
        std::cout << "Calibration screen size: " << calibration.screen_width << "x" << calibration.screen_height << std::endl;
        std::cout << "Calibration margin: " << calibration.margin_percent << "%" << std::endl;
    }

    if (fs::exists(calibration_path)) {
        std::cout << "Calibration file: " << calibration_path << std::endl;
    } else {
        std::cout << "Calibration file not found: " << calibration_path << std::endl;
    }

    if (!device.related_ids.empty()) {
        std::cout << "Current CTM matrices:" << std::endl;
        for (int id : device.related_ids) {
            auto matrix = readCtmForDevice(id);
            if (!matrix) {
                continue;
            }
            auto name_it = device.id_to_name.find(id);
            std::cout << "  Device " << id;
            if (name_it != device.id_to_name.end()) {
                std::cout << " (" << name_it->second << ")";
            }
            std::cout << std::endl;
            printMatrix(*matrix);
        }
    }

    if (fs::exists(profile_dir)) {
        std::cout << "Profiles directory: " << profile_dir << std::endl;
    }
}

} // namespace

void printHelp(const char* program) {
    std::cout << "Usage: " << program << " [options]" << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  -h, --help                Show this help" << std::endl;
    std::cout << "  -d, --device PATH        Path to input device" << std::endl;
    std::cout << "  --device-id ID           XInput device id" << std::endl;
    std::cout << "  -c, --calibrate          Run calibration" << std::endl;
    std::cout << "  -l, --load               Load calibration if available" << std::endl;
    std::cout << "  --list-devices           List input devices" << std::endl;
    std::cout << "  --list-monitors          List monitors" << std::endl;
    std::cout << "  --status                 Print current status (no event loop)" << std::endl;
    std::cout << "  -m, --monitor INDEX      Target monitor index for mapping" << std::endl;
    std::cout << "  --monitor-name NAME      Target monitor by name" << std::endl;
    std::cout << "  --map-full               Map to entire desktop" << std::endl;
    std::cout << "  --reset-ctm              Reset Coordinate Transformation Matrix" << std::endl;
    std::cout << "  --margin PERCENT         Dead-zone margin per side (default 0.5)" << std::endl;
    std::cout << "  --affine                 Use affine calibration fit" << std::endl;
    std::cout << "  --tool LIST              Comma separated tool filters (stylus,eraser,cursor,pad)" << std::endl;
    std::cout << "  --no-related-tools       Apply only to the specified device id" << std::endl;
    std::cout << "  --save-profile NAME      Save current mapping profile" << std::endl;
    std::cout << "  --load-profile NAME      Load mapping profile" << std::endl;
    std::cout << "  --list-profiles          List saved profiles" << std::endl;
    std::cout << "  --reapply                Allow profile application even if layout changed" << std::endl;
    std::cout << "  --config-dir PATH        Base directory for calibration/profile files" << std::endl;
    std::cout << "  --calibration-dir PATH   Override calibration directory" << std::endl;
    std::cout << "  --profile-dir PATH       Override profile directory" << std::endl;
    std::cout << "  --no-loop                Run setup and exit without reading events" << std::endl;
}

int main(int argc, char* argv[]) {
    std::string parse_error;
    Options options = parseArguments(argc, argv, parse_error);
    if (!parse_error.empty()) {
        std::cerr << parse_error << std::endl;
        return 1;
    }
    if (options.show_help) {
        printHelp(argv[0]);
        return 0;
    }

    if (std::getenv("WAYLAND_DISPLAY") && !std::getenv("DISPLAY")) {
        std::cerr << "Wayland session detected. xinput/xrandr mappings are X11-specific." << std::endl;
        return 1;
    }

    if (options.show_udev_instructions) {
        printUdevInstructions();
        if (!options.calibrate && !options.load_calibration && !options.load_profile &&
            !options.save_profile && !options.reset_mapping && !options.map_full_desktop &&
            options.monitor_index < 0 && options.monitor_name.empty()) {
            return 0;
        }
    }

    if (options.list_devices) {
        listDevices();
    }

    std::string layout_error;
    DesktopLayout layout = detectDesktopLayout(layout_error);
    if (!layout_error.empty()) {
        std::cerr << layout_error << std::endl;
        return 1;
    }

    if (options.list_monitors) {
        listMonitors(layout);
    }

    // Resolve storage directories
    fs::path exe_dir = fs::current_path();
    std::error_code symlink_ec;
    auto exe = fs::read_symlink("/proc/self/exe", symlink_ec);
    if (!symlink_ec && !exe.empty()) {
        exe_dir = exe.parent_path();
    }
    fs::path base_dir = options.config_dir.empty() ? exe_dir : fs::path(options.config_dir);
    fs::path calibration_dir = options.calibration_dir.empty() ? base_dir / "calibrations" : fs::path(options.calibration_dir);
    fs::path profile_dir = options.profile_dir.empty() ? base_dir / "profiles" : fs::path(options.profile_dir);

    std::error_code ec;
    fs::create_directories(calibration_dir, ec);
    fs::create_directories(profile_dir, ec);

    if (options.list_profiles) {
        listProfiles(profile_dir);
    }

    bool requires_device_operations = options.calibrate || options.load_calibration || options.load_profile ||
                                      options.save_profile || options.status || options.reset_mapping ||
                                      options.map_full_desktop || options.monitor_index >= 0 ||
                                      !options.monitor_name.empty();

    if (!requires_device_operations && (options.list_devices || options.list_monitors || options.list_profiles)) {
        options.run_event_loop = false;
    }

    DeviceContext device;
    if (options.device_id >= 0) {
        device.id = options.device_id;
        if (auto name = TouchScreen::DeviceHelper::getDeviceName(device.id)) {
            device.name = *name;
        }
        device.ranges = TouchScreen::DeviceHelper::getDeviceInfo(device.id);
        device.path = device.ranges.path;
        if (device.path.empty() && !options.device_path.empty()) {
            device.path = options.device_path;
        }
        if (device.name.empty() && !options.device_path.empty()) {
            device.name = options.device_path;
        }
        if (device.id >= 0) {
            if (options.include_related_tools) {
                device.related_ids = TouchScreen::DeviceHelper::findRelatedDeviceIds(device.id, true);
            } else {
                device.related_ids = {device.id};
            }
            for (int id : device.related_ids) {
                if (auto name = TouchScreen::DeviceHelper::getDeviceName(id)) {
                    device.id_to_name[id] = *name;
                }
            }
        }
    } else if (!options.device_path.empty()) {
        device.path = options.device_path;
    }

    bool device_required = requires_device_operations || options.run_event_loop;
    if (device.id < 0 && device.path.empty()) {
        if (device_required) {
            std::cerr << "No device specified. Use --device-id or --device." << std::endl;
            return 1;
        }
        return 0;
    }

    std::string device_display_name = !device.name.empty() ? device.name : device.path;
    std::string device_slug = slugify(device_display_name);
    if (device.id >= 0) {
        device_slug += "_id" + std::to_string(device.id);
    }
    fs::path calibration_path = calibration_dir / ("calibration_" + device_slug + ".ini");

    TouchReader reader;
    bool started = false;
    if (!device.path.empty()) {
        started = reader.Start(device.path);
    } else if (device.id >= 0) {
        if (!device.path.empty()) {
            started = reader.Start(device.path);
        } else {
            std::cerr << "Unable to determine device path for id " << device.id << std::endl;
        }
    }
    if (!started) {
        std::cerr << "Failed to open device. Ensure permissions allow reading input events." << std::endl;
        return 1;
    }

    std::cout << "Touch device: " << reader.GetSelectedDevice() << std::endl;
    if (device.ranges.max_x > 0 && device.ranges.max_y > 0) {
        std::cout << "Device coordinate range: 0-" << device.ranges.max_x << " x 0-" << device.ranges.max_y << std::endl;
    }

    if (options.calibrate) {
        int target_width = options.screen_width > 0 ? options.screen_width : layout.width;
        int target_height = options.screen_height > 0 ? options.screen_height : layout.height;
        std::cout << "Calibration target area: " << target_width << "x" << target_height << std::endl;
        CalibrationResult calib_result;
        if (!runCalibration(reader, target_width, target_height, options.margin_percent, options.use_affine, calib_result)) {
            std::cerr << "Calibration failed." << std::endl;
            reader.Stop();
            return 1;
        }
        if (options.use_affine) {
            reader.SetAffineCalibration(calib_result.affine, target_width, target_height);
        } else {
            reader.SetCalibration(static_cast<int>(std::lround(calib_result.min_x)),
                                  static_cast<int>(std::lround(calib_result.max_x)),
                                  static_cast<int>(std::lround(calib_result.min_y)),
                                  static_cast<int>(std::lround(calib_result.max_y)),
                                  target_width,
                                  target_height);
        }
        reader.SetCalibrationMargin(options.margin_percent);
        if (!reader.SaveCalibration(calibration_path.string())) {
            std::cerr << "Failed to save calibration to " << calibration_path << std::endl;
        } else {
            TouchScreen::Config::IniData data;
            if (TouchScreen::Config::LoadIni(calibration_path.string(), data)) {
                TouchScreen::Config::SetValue(data, "Metadata", "device_id", std::to_string(device.id));
                TouchScreen::Config::SetValue(data, "Metadata", "device_name", device_display_name);
                TouchScreen::Config::SetValue(data, "Metadata", "layout_hash", layout.hash);
                TouchScreen::Config::SaveIni(calibration_path.string(), data);
            }
            std::cout << "Calibration saved: " << calibration_path << std::endl;
        }
        options.load_calibration = false; // Already applied
    }

    if (options.load_calibration || (!options.calibrate && fs::exists(calibration_path))) {
        if (reader.LoadCalibration(calibration_path.string())) {
            std::cout << "Loaded calibration from " << calibration_path << std::endl;
        } else if (options.load_calibration) {
            std::cerr << "Failed to load calibration from " << calibration_path << std::endl;
        }
    }

    std::vector<int> target_device_ids = device.related_ids.empty() ? std::vector<int>{device.id} : device.related_ids;
    target_device_ids = filterDeviceIdsByTool(target_device_ids, device.id_to_name, options.tool_filters);

    bool mapping_applied = false;
    if (options.reset_mapping) {
        std::string error;
        if (!applyCtmToDevices(target_device_ids, identityMatrix(), error)) {
            std::cerr << error << std::endl;
        } else {
            std::cout << "Coordinate Transformation Matrix reset to identity." << std::endl;
            mapping_applied = true;
        }
    }

    if (options.load_profile) {
        ProfileData profile;
        fs::path profile_path = profile_dir / (slugify(options.profile_to_load) + ".ini");
        if (!fs::exists(profile_path)) {
            std::cerr << "Profile not found: " << profile_path << std::endl;
            reader.Stop();
            return 1;
        }
        std::string error;
        if (!loadProfile(profile_path, profile, error)) {
            std::cerr << error << std::endl;
            reader.Stop();
            return 1;
        }
        if (!options.reapply && !profile.layout_hash.empty() && profile.layout_hash != layout.hash) {
            std::cerr << "Current layout hash " << layout.hash << " differs from profile hash " << profile.layout_hash << ". Use --reapply to override." << std::endl;
            reader.Stop();
            return 1;
        }
        if (device.id < 0) {
            std::cerr << "Profile application requires --device-id." << std::endl;
            reader.Stop();
            return 1;
        }
        if (profile.include_related) {
            target_device_ids = TouchScreen::DeviceHelper::findRelatedDeviceIds(device.id, true);
        } else {
            target_device_ids = {device.id};
        }
        target_device_ids = filterDeviceIdsByTool(target_device_ids, device.id_to_name, profile.tool_filters);
        for (int id : target_device_ids) {
            if (!device.id_to_name.count(id)) {
                if (auto name = TouchScreen::DeviceHelper::getDeviceName(id)) {
                    device.id_to_name[id] = *name;
                }
            }
        }
        std::optional<MonitorInfo> targetMonitor;
        if (!profile.monitor.name.empty()) {
            targetMonitor = findMonitorByName(layout, profile.monitor.name);
        }
        if (!targetMonitor) {
            targetMonitor = findMonitorByIndex(layout, profile.monitor.index);
        }
        if (!targetMonitor) {
            std::cerr << "Unable to resolve monitor for profile." << std::endl;
            reader.Stop();
            return 1;
        }
        auto matrix = computeCtm(layout, *targetMonitor);
        std::string error_apply;
        if (!applyCtmToDevices(target_device_ids, matrix, error_apply)) {
            std::cerr << error_apply << std::endl;
        } else {
            std::cout << "Applied profile " << profile.name << " to monitor " << targetMonitor->name << std::endl;
            printMatrix(matrix);
            mapping_applied = true;
        }
    }

    if (!options.profile_to_save.empty()) {
        ProfileData profile;
        profile.name = options.profile_to_save;
        profile.device_id = device.id;
        profile.device_name = device_display_name;
        profile.include_related = options.include_related_tools;
        profile.tool_filters = options.tool_filters;
        profile.layout_hash = layout.hash;
        auto targetMonitor = options.monitor_name.empty()
            ? findMonitorByIndex(layout, options.monitor_index)
            : findMonitorByName(layout, options.monitor_name);
        if (!targetMonitor) {
            targetMonitor = layout.monitors.empty() ? std::nullopt : std::optional<MonitorInfo>(layout.monitors.front());
        }
        if (targetMonitor) {
            profile.monitor = *targetMonitor;
            profile.layout_snapshot = layout;
            profile.ctm = computeCtm(layout, *targetMonitor);
            std::string error;
            fs::path profile_path = profile_dir / (slugify(profile.name) + ".ini");
            if (saveProfile(profile_path, profile, error)) {
                std::cout << "Profile saved: " << profile_path << std::endl;
            } else {
                std::cerr << error << std::endl;
            }
        }
    }

    if (!options.monitor_name.empty() || options.monitor_index >= 0 || options.map_full_desktop) {
        std::optional<MonitorInfo> targetMonitor;
        if (options.map_full_desktop) {
            // full desktop mapping uses identity matrix but we still print info
            targetMonitor = std::nullopt;
        } else if (!options.monitor_name.empty()) {
            targetMonitor = findMonitorByName(layout, options.monitor_name);
        } else {
            targetMonitor = findMonitorByIndex(layout, options.monitor_index);
        }
        std::array<double, 9> matrix = identityMatrix();
        if (!options.map_full_desktop && !targetMonitor) {
            std::cerr << "Unable to resolve monitor selection." << std::endl;
        } else if (targetMonitor) {
            matrix = computeCtm(layout, *targetMonitor);
        }
        std::string error;
        if (!applyCtmToDevices(target_device_ids, matrix, error)) {
            std::cerr << error << std::endl;
        } else {
            if (targetMonitor) {
                std::cout << "Applied mapping to monitor " << targetMonitor->name << std::endl;
            } else {
                std::cout << "Applied full-desktop mapping." << std::endl;
            }
            printMatrix(matrix);
            mapping_applied = true;
        }
    }

    if (options.status) {
        auto currentCalibration = reader.GetCalibration();
        printStatus(layout, device, calibration_path, profile_dir, currentCalibration);
    }

    if (!options.run_event_loop) {
        reader.Stop();
        return mapping_applied ? 0 : 0;
    }

    signal(SIGINT, signalHandler);
    reader.SetEventCallback([](const TouchScreen::TouchEvent& event) {
        static const char* names[] = {
            "Down", "Up", "Move", "SwipeLeft", "SwipeRight", "SwipeUp", "SwipeDown",
            "PinchIn", "PinchOut", "LongPress", "DoubleTap", "Rotate"
        };
        int typeIndex = static_cast<int>(event.type);
        const char* typeName = (typeIndex >= 0 && typeIndex < static_cast<int>(std::size(names))) ? names[typeIndex] : "Unknown";
        std::cout << "Event: " << typeName << " x=" << event.x << " y=" << event.y << " touches=" << event.touch_count << std::endl;
    });

    std::cout << "Press Ctrl+C to exit" << std::endl;
    while (g_running) {
        std::this_thread::sleep_for(100ms);
    }

    reader.Stop();
    return 0;
}
