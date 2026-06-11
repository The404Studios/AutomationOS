#!/usr/bin/autoshell
#
# AutomationOS System Monitoring Script
#
# Continuous monitoring of system resources with alerts
# Monitors: CPU, memory, disk, network, processes
#
# Usage: monitor.sh [options]
#   -i, --interval N  Update interval in seconds (default: 5)
#   -t, --threshold   CPU threshold % (default: 80)
#   -m, --memory      Memory threshold % (default: 85)
#   -d, --disk        Disk threshold % (default: 90)
#   -l, --log FILE    Log file (default: /var/log/monitor.log)
#   -a, --alert CMD   Alert command to run on threshold
#   -h, --help        Show this help
#

# Default configuration
INTERVAL=5
CPU_THRESHOLD=80
MEM_THRESHOLD=85
DISK_THRESHOLD=90
LOG_FILE="/var/log/monitor.log"
ALERT_CMD=""
CONTINUOUS=1

# Colors
COLOR_RESET="\033[0m"
COLOR_RED="\033[31m"
COLOR_GREEN="\033[32m"
COLOR_YELLOW="\033[33m"
COLOR_BLUE="\033[34m"
COLOR_CYAN="\033[36m"
COLOR_BOLD="\033[1m"

#=============================================================================
# Functions
#=============================================================================

show_help() {
    cat << EOF
AutomationOS System Monitoring Script

Usage: monitor.sh [options]

Options:
  -i, --interval N    Update interval in seconds (default: 5)
  -t, --threshold N   CPU threshold % (default: 80)
  -m, --memory N      Memory threshold % (default: 85)
  -d, --disk N        Disk threshold % (default: 90)
  -l, --log FILE      Log file (default: /var/log/monitor.log)
  -a, --alert CMD     Alert command to run on threshold
  -o, --once          Run once and exit (don't loop)
  -h, --help          Show this help

Examples:
  monitor.sh                                    # Start monitoring
  monitor.sh -i 10 -t 90                        # 10s interval, 90% CPU threshold
  monitor.sh -a "echo 'Alert!' | mail admin"    # Send email alerts
  monitor.sh -o                                 # One-shot monitoring

EOF
    exit 0
}

parse_args() {
    while [ $# -gt 0 ]; do
        case "$1" in
            -i|--interval)
                INTERVAL="$2"
                shift 2
                ;;
            -t|--threshold)
                CPU_THRESHOLD="$2"
                shift 2
                ;;
            -m|--memory)
                MEM_THRESHOLD="$2"
                shift 2
                ;;
            -d|--disk)
                DISK_THRESHOLD="$2"
                shift 2
                ;;
            -l|--log)
                LOG_FILE="$2"
                shift 2
                ;;
            -a|--alert)
                ALERT_CMD="$2"
                shift 2
                ;;
            -o|--once)
                CONTINUOUS=0
                shift
                ;;
            -h|--help)
                show_help
                ;;
            *)
                echo "Unknown option: $1"
                exit 1
                ;;
        esac
    done
}

log_event() {
    local level="$1"
    local message="$2"
    local timestamp=$(date '+%Y-%m-%d %H:%M:%S')

    echo "$timestamp [$level] $message" >> "$LOG_FILE"
}

alert() {
    local message="$1"

    log_event "ALERT" "$message"

    if [ -n "$ALERT_CMD" ]; then
        eval "$ALERT_CMD '$message'"
    fi
}

get_cpu_usage() {
    # Read /proc/stat for CPU usage
    if [ ! -f /proc/stat ]; then
        echo "0"
        return
    fi

    # Simple CPU calculation
    local cpu_line=$(grep '^cpu ' /proc/stat)
    local user=$(echo $cpu_line | awk '{print $2}')
    local nice=$(echo $cpu_line | awk '{print $3}')
    local system=$(echo $cpu_line | awk '{print $4}')
    local idle=$(echo $cpu_line | awk '{print $5}')

    local total=$((user + nice + system + idle))
    local usage=$((total - idle))

    if [ $total -gt 0 ]; then
        local cpu_percent=$((usage * 100 / total))
        echo "$cpu_percent"
    else
        echo "0"
    fi
}

get_memory_usage() {
    if [ ! -f /proc/meminfo ]; then
        echo "0"
        return
    fi

    local total=$(grep MemTotal /proc/meminfo | awk '{print $2}')
    local available=$(grep MemAvailable /proc/meminfo | awk '{print $2}')

    if [ $total -gt 0 ]; then
        local used=$((total - available))
        local mem_percent=$((used * 100 / total))
        echo "$mem_percent"
    else
        echo "0"
    fi
}

get_disk_usage() {
    local mount_point="${1:-/}"

    local usage=$(df -h "$mount_point" | tail -1 | awk '{print $5}' | sed 's/%//')

    if [ -z "$usage" ]; then
        usage="0"
    fi

    echo "$usage"
}

get_network_stats() {
    if [ ! -f /proc/net/dev ]; then
        echo "0 0"
        return
    fi

    # Sum all interfaces
    local rx_bytes=0
    local tx_bytes=0

    while read line; do
        if echo "$line" | grep -q ":"; then
            local rx=$(echo "$line" | awk '{print $2}')
            local tx=$(echo "$line" | awk '{print $10}')
            rx_bytes=$((rx_bytes + rx))
            tx_bytes=$((tx_bytes + tx))
        fi
    done < <(tail -n +3 /proc/net/dev)

    echo "$rx_bytes $tx_bytes"
}

format_bytes() {
    local bytes=$1

    if [ $bytes -lt 1024 ]; then
        echo "${bytes}B"
    elif [ $bytes -lt 1048576 ]; then
        echo "$((bytes / 1024))KB"
    elif [ $bytes -lt 1073741824 ]; then
        echo "$((bytes / 1048576))MB"
    else
        echo "$((bytes / 1073741824))GB"
    fi
}

get_load_average() {
    if [ -f /proc/loadavg ]; then
        cat /proc/loadavg | awk '{print $1, $2, $3}'
    else
        echo "0.00 0.00 0.00"
    fi
}

get_process_count() {
    if [ -d /proc ]; then
        ls -d /proc/[0-9]* 2>/dev/null | wc -l
    else
        echo "0"
    fi
}

get_uptime() {
    if [ -f /proc/uptime ]; then
        local uptime_sec=$(cat /proc/uptime | awk '{print int($1)}')
        local days=$((uptime_sec / 86400))
        local hours=$(( (uptime_sec % 86400) / 3600 ))
        local mins=$(( (uptime_sec % 3600) / 60 ))

        echo "${days}d ${hours}h ${mins}m"
    else
        echo "Unknown"
    fi
}

draw_bar() {
    local value=$1
    local width=30
    local filled=$((value * width / 100))

    local bar=""
    local i

    for ((i=0; i<filled; i++)); do
        bar="${bar}█"
    done

    for ((i=filled; i<width; i++)); do
        bar="${bar}░"
    done

    # Color based on threshold
    if [ $value -ge 90 ]; then
        echo -e "${COLOR_RED}${bar}${COLOR_RESET}"
    elif [ $value -ge 70 ]; then
        echo -e "${COLOR_YELLOW}${bar}${COLOR_RESET}"
    else
        echo -e "${COLOR_GREEN}${bar}${COLOR_RESET}"
    fi
}

display_dashboard() {
    local cpu_usage=$(get_cpu_usage)
    local mem_usage=$(get_memory_usage)
    local disk_usage=$(get_disk_usage "/")
    local load_avg=$(get_load_average)
    local process_count=$(get_process_count)
    local uptime=$(get_uptime)
    local timestamp=$(date '+%Y-%m-%d %H:%M:%S')

    # Network stats
    local net_stats=$(get_network_stats)
    local rx_bytes=$(echo $net_stats | awk '{print $1}')
    local tx_bytes=$(echo $net_stats | awk '{print $2}')

    # Clear screen
    clear

    # Header
    echo -e "${COLOR_BOLD}${COLOR_CYAN}"
    echo "═══════════════════════════════════════════════════════════════════════"
    echo "  AutomationOS System Monitor"
    echo "═══════════════════════════════════════════════════════════════════════"
    echo -e "${COLOR_RESET}"

    # System info
    echo -e "${COLOR_BLUE}Hostname:${COLOR_RESET} $(hostname)"
    echo -e "${COLOR_BLUE}Uptime:${COLOR_RESET}   $uptime"
    echo -e "${COLOR_BLUE}Time:${COLOR_RESET}     $timestamp"
    echo ""

    # CPU
    echo -e "${COLOR_BOLD}CPU Usage:${COLOR_RESET} $cpu_usage%"
    draw_bar $cpu_usage
    echo ""

    # Memory
    echo -e "${COLOR_BOLD}Memory Usage:${COLOR_RESET} $mem_usage%"
    draw_bar $mem_usage

    if [ -f /proc/meminfo ]; then
        local total_mb=$(($(grep MemTotal /proc/meminfo | awk '{print $2}') / 1024))
        local avail_mb=$(($(grep MemAvailable /proc/meminfo | awk '{print $2}') / 1024))
        echo -e "  Total: ${total_mb}MB  |  Available: ${avail_mb}MB"
    fi
    echo ""

    # Disk
    echo -e "${COLOR_BOLD}Disk Usage (root):${COLOR_RESET} $disk_usage%"
    draw_bar $disk_usage

    if command -v df >/dev/null 2>&1; then
        echo "  $(df -h / | tail -1 | awk '{print "Used: "$3" / "$2" (Available: "$4")"}')"
    fi
    echo ""

    # Load average
    echo -e "${COLOR_BOLD}Load Average:${COLOR_RESET} $load_avg"
    echo ""

    # Processes
    echo -e "${COLOR_BOLD}Processes:${COLOR_RESET} $process_count"
    echo ""

    # Network
    echo -e "${COLOR_BOLD}Network:${COLOR_RESET}"
    echo "  RX: $(format_bytes $rx_bytes)"
    echo "  TX: $(format_bytes $tx_bytes)"
    echo ""

    # Top processes (if available)
    if command -v ps >/dev/null 2>&1; then
        echo -e "${COLOR_BOLD}Top CPU Processes:${COLOR_RESET}"
        ps aux --sort=-pcpu | head -6 | tail -5 | awk '{printf "  %-20s %5s%%  %s\n", substr($11,1,20), $3, $2}'
        echo ""
    fi

    # Alerts
    local alerts_shown=0

    if [ $cpu_usage -ge $CPU_THRESHOLD ]; then
        echo -e "${COLOR_RED}⚠ ALERT: High CPU usage ($cpu_usage%)${COLOR_RESET}"
        alert "High CPU usage: $cpu_usage%"
        alerts_shown=1
    fi

    if [ $mem_usage -ge $MEM_THRESHOLD ]; then
        echo -e "${COLOR_RED}⚠ ALERT: High memory usage ($mem_usage%)${COLOR_RESET}"
        alert "High memory usage: $mem_usage%"
        alerts_shown=1
    fi

    if [ $disk_usage -ge $DISK_THRESHOLD ]; then
        echo -e "${COLOR_RED}⚠ ALERT: High disk usage ($disk_usage%)${COLOR_RESET}"
        alert "High disk usage: $disk_usage%"
        alerts_shown=1
    fi

    if [ $alerts_shown -eq 0 ]; then
        echo -e "${COLOR_GREEN}✓ All systems normal${COLOR_RESET}"
    fi

    echo ""
    echo "═══════════════════════════════════════════════════════════════════════"

    if [ $CONTINUOUS -eq 1 ]; then
        echo -e "Refreshing every ${INTERVAL}s... (Press Ctrl+C to stop)"
    fi
}

#=============================================================================
# Main
#=============================================================================

main() {
    parse_args "$@"

    # Ensure log directory exists
    local log_dir=$(dirname "$LOG_FILE")
    mkdir -p "$log_dir" 2>/dev/null

    log_event "INFO" "Monitoring started"

    # Display dashboard
    if [ $CONTINUOUS -eq 1 ]; then
        while true; do
            display_dashboard
            sleep $INTERVAL
        done
    else
        display_dashboard
    fi

    log_event "INFO" "Monitoring stopped"
}

# Handle Ctrl+C gracefully
trap 'echo ""; log_event "INFO" "Monitoring stopped by user"; exit 0' INT TERM

# Run main
main "$@"
