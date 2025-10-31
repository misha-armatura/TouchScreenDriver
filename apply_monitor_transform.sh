#!/bin/bash

# apply_monitor_transform.sh - Configure touchscreen monitor mapping
# This script applies coordinate transformation matrix for specific monitors

# Default values
MONITOR_INDEX=-1
DEVICE_NAME=""
DRY_RUN=false
VERBOSE=false

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

print_usage() {
    echo "Usage: $0 [options]"
    echo "Options:"
    echo "  -m, --monitor <index>    Monitor index (0 to N-1 for specific monitor, -1=full desktop)"
    echo "  -d, --device <name>      Device name (optional, auto-detect if not specified)"
    echo "  -n, --dry-run            Show what would be done without executing"
    echo "  -v, --verbose            Verbose output"
    echo "  -h, --help               Show this help"
    echo ""
    echo "Examples:"
    echo "  $0 --monitor 1                    # Map to second monitor"
    echo "  $0 --monitor 0 --device 'Touch'   # Map specific device to first monitor"
    echo "  $0 --monitor -1                   # Map to full desktop (reset)"
}

log_info() {
    if [ "$VERBOSE" = true ]; then
        echo -e "${GREEN}[INFO]${NC} $1"
    fi
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -m|--monitor)
            MONITOR_INDEX="$2"
            shift 2
            ;;
        -d|--device)
            DEVICE_NAME="$2"
            shift 2
            ;;
        -n|--dry-run)
            DRY_RUN=true
            shift
            ;;
        -v|--verbose)
            VERBOSE=true
            shift
            ;;
        -h|--help)
            print_usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            print_usage
            exit 1
            ;;
    esac
done

# Validate monitor index
if [[ ! "$MONITOR_INDEX" =~ ^-?[0-9]+$ ]]; then
    log_error "Invalid monitor index: $MONITOR_INDEX"
    exit 1
fi

log_info "Starting monitor transformation for monitor $MONITOR_INDEX"

# Get monitor information using xrandr
get_monitor_info() {
    local monitor_index=$1
    
    # Get connected monitors
    local monitors=($(xrandr --listmonitors | grep -E "^ [0-9]:" | awk '{print $4}'))
    local geometries=($(xrandr --listmonitors | grep -E "^ [0-9]:" | awk '{print $3}'))
    
    if [ $monitor_index -eq -1 ]; then
        # Full desktop mode
        echo "full_desktop"
        return 0
    fi
    
    if [ $monitor_index -ge ${#monitors[@]} ]; then
        log_error "Monitor index $monitor_index not found. Available monitors: 0-$((${#monitors[@]}-1))"
        return 1
    fi
    
    echo "${monitors[$monitor_index]} ${geometries[$monitor_index]}"
    return 0
}

# Get touchscreen devices
get_touch_devices() {
    xinput list | grep -i -E "touch|wacom" | grep -v "Virtual core" | awk -F'[=\t]' '{print $2}' | sort -u
}

# Apply transformation matrix
apply_transform() {
    local device_id=$1
    local monitor_info=$2
    
    if [ "$monitor_info" = "full_desktop" ]; then
        # Reset to full desktop
        local matrix="1 0 0 0 1 0 0 0 1"
        log_info "Resetting device $device_id to full desktop"
    else
        # Parse monitor geometry from xrandr output
        local monitor_name=$(echo "$monitor_info" | awk '{print $1}')
        local resolution=$(xrandr | grep "^$monitor_name" | grep -oP '\d+x\d+\+\d+\+\d+' | head -1)
        
        if [ -z "$resolution" ]; then
            log_error "Failed to get resolution for monitor $monitor_name"
            return 1
        fi
        
        local width=$(echo "$resolution" | cut -d'x' -f1)
        local height=$(echo "$resolution" | cut -d'x' -f2 | cut -d'+' -f1)
        local x_offset=$(echo "$resolution" | cut -d'+' -f2)
        local y_offset=$(echo "$resolution" | cut -d'+' -f3)
        
        log_info "Parsed resolution: ${width}x${height}+${x_offset}+${y_offset}"
        
        # Get total desktop size
        local desktop_info=$(xrandr | grep "current" | awk '{print $8 "x" $10}' | tr -d ',')
        local desktop_width=$(echo "$desktop_info" | cut -d'x' -f1)
        local desktop_height=$(echo "$desktop_info" | cut -d'x' -f2)
        
        # Calculate transformation matrix
        local c0=$(echo "scale=6; $width / $desktop_width" | bc -l)
        local c2=$(echo "scale=6; $x_offset / $desktop_width" | bc -l)
        local c4=$(echo "scale=6; $height / $desktop_height" | bc -l)
        local c5=$(echo "scale=6; $y_offset / $desktop_height" | bc -l)
        
        local matrix="$c0 0 $c2 0 $c4 $c5 0 0 1"
        log_info "Mapping device $device_id to monitor ($width x $height at +$x_offset+$y_offset)"
        log_info "Desktop size: $desktop_width x $desktop_height"
        log_info "Transformation matrix: $matrix"
    fi
    
    if [ "$DRY_RUN" = true ]; then
        echo "Would execute: xinput set-prop $device_id 'Coordinate Transformation Matrix' $matrix"
    else
        xinput set-prop "$device_id" "Coordinate Transformation Matrix" $matrix
        if [ $? -eq 0 ]; then
            log_info "Successfully applied transformation to device $device_id"
        else
            log_error "Failed to apply transformation to device $device_id"
            return 1
        fi
    fi
}

# Check if bc is installed
if ! command -v bc &> /dev/null; then
    log_error "bc calculator is required but not installed. Please install it:"
    log_error "  Ubuntu/Debian: sudo apt install bc"
    log_error "  Fedora/RHEL: sudo dnf install bc"
    exit 1
fi

# Get monitor information
monitor_info=$(get_monitor_info $MONITOR_INDEX)
if [ $? -ne 0 ]; then
    exit 1
fi

log_info "Monitor info: $monitor_info"

# Get touchscreen devices
if [ -z "$DEVICE_NAME" ]; then
    # Auto-detect touchscreen devices
    touch_devices=$(get_touch_devices)
    if [ -z "$touch_devices" ]; then
        log_warn "No touchscreen devices found. Trying to find any input devices with 'touch' in name..."
        touch_devices=$(xinput list | grep -i touch | awk -F'id=' '{print $2}' | awk '{print $1}')
    fi
else
    # Find specific device
    touch_devices=$(xinput list | grep -i "$DEVICE_NAME" | awk -F'id=' '{print $2}' | awk '{print $1}')
fi

if [ -z "$touch_devices" ]; then
    log_error "No touchscreen devices found"
    log_info "Available input devices:"
    xinput list | grep -v "Virtual core"
    exit 1
fi

# Apply transformation to each device
success_count=0
total_count=0

for device_id in $touch_devices; do
    total_count=$((total_count + 1))
    device_name=$(xinput list | grep "id=$device_id" | awk -F'\t' '{print $1}' | sed 's/^[[:space:]]*//')
    log_info "Processing device: $device_name (ID: $device_id)"
    
    if apply_transform "$device_id" "$monitor_info"; then
        success_count=$((success_count + 1))
    fi
done

log_info "Processed $total_count devices, $success_count successful"

if [ $success_count -gt 0 ]; then
    log_info "Transformation applied successfully"
    exit 0
else
    log_error "No transformations were applied successfully"
    exit 1
fi