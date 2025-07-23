#!/bin/bash

# test_cxl_sender.sh - CXL Message Sender Test Script
# Usage examples for the CXL message sender test module

set -e

MODULE_DIR="/home/comsys/CXLSHMSWcoherence/msg_layer_module"
CXL_SHM_MODULE="cxl_shm.ko"
TEST_MODULE="test_cxl_sender.ko"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

print_header() {
    echo -e "${BLUE}=== $1 ===${NC}"
}

print_success() {
    echo -e "${GREEN}✓ $1${NC}"
}

print_warning() {
    echo -e "${YELLOW}⚠ $1${NC}"
}

print_error() {
    echo -e "${RED}✗ $1${NC}"
}

check_modules() {
    print_header "Checking Modules"
    
    if [ ! -f "$MODULE_DIR/$CXL_SHM_MODULE" ]; then
        print_error "CXL SHM module not found: $MODULE_DIR/$CXL_SHM_MODULE"
        exit 1
    fi
    
    if [ ! -f "$MODULE_DIR/$TEST_MODULE" ]; then
        print_error "Test module not found: $MODULE_DIR/$TEST_MODULE"
        exit 1
    fi
    
    print_success "All module files found"
}

load_cxl_shm() {
    local node_id=${1:-0}
    local dax_name=${2:-"dax0.0"}
    
    print_header "Loading CXL SHM Module"
    
    if lsmod | grep -q "cxl_shm"; then
        print_warning "CXL SHM module already loaded"
        return 0
    fi
    
    echo "Loading cxl_shm.ko with node_id=$node_id dax_name=$dax_name"
    if sudo insmod "$MODULE_DIR/$CXL_SHM_MODULE" node_id=$node_id dax_name=$dax_name; then
        print_success "CXL SHM module loaded successfully"
        sleep 2  # Wait for initialization
    else
        print_error "Failed to load CXL SHM module"
        exit 1
    fi
}

unload_modules() {
    print_header "Unloading Modules"
    
    if lsmod | grep -q "test_cxl_sender"; then
        echo "Unloading test_cxl_sender..."
        sudo rmmod test_cxl_sender || print_warning "Failed to unload test_cxl_sender"
    fi
    
    if lsmod | grep -q "cxl_shm"; then
        echo "Unloading cxl_shm..."
        sudo rmmod cxl_shm || print_warning "Failed to unload cxl_shm"
    fi
    
    print_success "Modules unloaded"
}

run_test() {
    local test_name="$1"
    local params="$2"
    local duration=${3:-30}
    
    print_header "Running Test: $test_name"
    
    if lsmod | grep -q "test_cxl_sender"; then
        print_warning "Test module already loaded, unloading first..."
        sudo rmmod test_cxl_sender || true
        sleep 1
    fi
    
    echo "Loading test module with parameters: $params"
    if sudo insmod "$MODULE_DIR/$TEST_MODULE" $params; then
        print_success "Test module loaded"
        
        echo "Monitoring logs for ${duration} seconds..."
        echo "Press Ctrl+C to stop monitoring early"
        
        # Monitor logs for specified duration
        timeout ${duration}s sudo dmesg -w | grep "CXL_SENDER" || true
        
        echo ""
        print_header "Test Statistics"
        sudo dmesg | grep "CXL_SENDER.*Final statistics" | tail -1 || print_warning "No final statistics found"
        
        # Unload test module
        sudo rmmod test_cxl_sender 2>/dev/null || print_warning "Test module already unloaded"
        
    else
        print_error "Failed to load test module"
        return 1
    fi
}

show_help() {
    echo "CXL Message Sender Test Script"
    echo ""
    echo "Usage: $0 [COMMAND] [OPTIONS]"
    echo ""
    echo "Commands:"
    echo "  setup [node_id] [dax_name]     - Load CXL SHM module"
    echo "  cleanup                        - Unload all modules"
    echo "  test1                          - Basic ping test (target_node=1)"
    echo "  test2                          - Data transfer test (target_node=2)"
    echo "  test3                          - High frequency test"
    echo "  test4                          - Broadcast test"
    echo "  test5                          - Long duration test"
    echo "  custom [params]                - Custom test with specified parameters"
    echo "  monitor                        - Monitor CXL messages in real-time"
    echo "  status                         - Show module status"
    echo "  help                           - Show this help"
    echo ""
    echo "Examples:"
    echo "  $0 setup 0 dax0.0"
    echo "  $0 test1"
    echo "  $0 custom 'target_node=2 message_count=20 send_interval=1'"
    echo "  $0 cleanup"
}

monitor_messages() {
    print_header "Real-time Message Monitoring"
    echo "Press Ctrl+C to stop monitoring"
    echo ""
    sudo dmesg -w | grep -E "(CXL_SENDER|shm_cxl)" --color=always
}

show_status() {
    print_header "Module Status"
    
    echo "Loaded modules:"
    lsmod | grep -E "(cxl_shm|test_cxl_sender)" || echo "No CXL modules loaded"
    
    echo ""
    echo "Recent CXL messages:"
    sudo dmesg | grep -E "(CXL_SENDER|shm_cxl)" | tail -10 || echo "No recent CXL messages"
}

# Main script logic
cd "$MODULE_DIR"

case "$1" in
    setup)
        check_modules
        load_cxl_shm "$2" "$3"
        ;;
    cleanup)
        unload_modules
        ;;
    test1)
        check_modules
        run_test "Basic Ping Test" "target_node=1 message_count=5 send_interval=3" 20
        ;;
    test2)
        check_modules
        run_test "Data Transfer Test" "target_node=2 message_count=8 send_interval=2" 25
        ;;
    test3)
        check_modules
        run_test "High Frequency Test" "target_node=1 message_count=15 send_interval=1" 20
        ;;
    test4)
        check_modules
        run_test "Broadcast Test" "enable_broadcast=true message_count=5 send_interval=3" 20
        ;;
    test5)
        check_modules
        run_test "Long Duration Test" "target_node=3 message_count=20 send_interval=2" 50
        ;;
    custom)
        check_modules
        run_test "Custom Test" "$2" ${3:-30}
        ;;
    monitor)
        monitor_messages
        ;;
    status)
        show_status
        ;;
    help|--help|-h)
        show_help
        ;;
    *)
        echo "Unknown command: $1"
        echo "Use '$0 help' for usage information"
        exit 1
        ;;
esac
