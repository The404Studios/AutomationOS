#!/usr/bin/autoshell
#
# AutomationOS System Backup Script
#
# Automated backup of system files and user data
# Supports incremental backups, compression, and rotation
#
# Usage: backup.sh [options]
#   -f, --full        Full backup (default: incremental)
#   -d, --dest DIR    Backup destination (default: /backup)
#   -r, --rotate N    Keep last N backups (default: 7)
#   -c, --compress    Compress backups (default: yes)
#   -v, --verbose     Verbose output
#   -h, --help        Show this help
#

# Default configuration
BACKUP_DEST="/backup"
BACKUP_TYPE="incremental"
ROTATION_COUNT=7
COMPRESS=1
VERBOSE=0

# Directories to backup
BACKUP_SOURCES=(
    "/home"
    "/etc"
    "/var/log"
    "/opt"
)

# Exclude patterns
EXCLUDE_PATTERNS=(
    "*.tmp"
    "*.cache"
    "*.log"
    "/home/*/.cache"
    "/home/*/Downloads"
)

# Colors
COLOR_RED="\033[31m"
COLOR_GREEN="\033[32m"
COLOR_YELLOW="\033[33m"
COLOR_BLUE="\033[34m"
COLOR_RESET="\033[0m"

#=============================================================================
# Functions
#=============================================================================

log() {
    local level="$1"
    shift
    local message="$*"
    local timestamp=$(date '+%Y-%m-%d %H:%M:%S')

    case "$level" in
        INFO)
            echo -e "${COLOR_BLUE}[INFO]${COLOR_RESET} $timestamp - $message"
            ;;
        SUCCESS)
            echo -e "${COLOR_GREEN}[SUCCESS]${COLOR_RESET} $timestamp - $message"
            ;;
        WARNING)
            echo -e "${COLOR_YELLOW}[WARNING]${COLOR_RESET} $timestamp - $message"
            ;;
        ERROR)
            echo -e "${COLOR_RED}[ERROR]${COLOR_RESET} $timestamp - $message"
            ;;
    esac

    # Log to file
    echo "$timestamp [$level] $message" >> "$BACKUP_DEST/backup.log"
}

show_help() {
    cat << EOF
AutomationOS System Backup Script

Usage: backup.sh [options]

Options:
  -f, --full        Full backup (default: incremental)
  -d, --dest DIR    Backup destination (default: /backup)
  -r, --rotate N    Keep last N backups (default: 7)
  -c, --compress    Compress backups (default: yes)
  -v, --verbose     Verbose output
  -h, --help        Show this help

Examples:
  backup.sh                        # Incremental backup to /backup
  backup.sh -f -d /mnt/external    # Full backup to external drive
  backup.sh -r 14                  # Keep 14 days of backups

EOF
    exit 0
}

parse_args() {
    while [ $# -gt 0 ]; do
        case "$1" in
            -f|--full)
                BACKUP_TYPE="full"
                shift
                ;;
            -d|--dest)
                BACKUP_DEST="$2"
                shift 2
                ;;
            -r|--rotate)
                ROTATION_COUNT="$2"
                shift 2
                ;;
            -c|--compress)
                COMPRESS=1
                shift
                ;;
            -v|--verbose)
                VERBOSE=1
                shift
                ;;
            -h|--help)
                show_help
                ;;
            *)
                echo "Unknown option: $1"
                echo "Use --help for usage information"
                exit 1
                ;;
        esac
    done
}

check_prerequisites() {
    log INFO "Checking prerequisites..."

    # Check if backup destination exists
    if [ ! -d "$BACKUP_DEST" ]; then
        log INFO "Creating backup destination: $BACKUP_DEST"
        mkdir -p "$BACKUP_DEST" || {
            log ERROR "Failed to create backup destination"
            exit 1
        }
    fi

    # Check disk space
    local available=$(df -k "$BACKUP_DEST" | tail -1 | awk '{print $4}')
    local required=10485760  # 10 GB in KB

    if [ "$available" -lt "$required" ]; then
        log WARNING "Low disk space: $(($available / 1024)) MB available"
    fi

    # Check if running as root
    if [ "$(id -u)" -ne 0 ]; then
        log WARNING "Not running as root. Some files may be skipped."
    fi

    log SUCCESS "Prerequisites check completed"
}

create_backup() {
    local timestamp=$(date '+%Y%m%d_%H%M%S')
    local backup_dir="$BACKUP_DEST/backup_$timestamp"

    log INFO "Creating $BACKUP_TYPE backup: $backup_dir"

    # Create backup directory
    mkdir -p "$backup_dir" || {
        log ERROR "Failed to create backup directory"
        return 1
    }

    # Build tar options
    local tar_opts=""
    if [ $VERBOSE -eq 1 ]; then
        tar_opts="$tar_opts -v"
    fi

    # Build exclude options
    local exclude_opts=""
    for pattern in "${EXCLUDE_PATTERNS[@]}"; do
        exclude_opts="$exclude_opts --exclude=$pattern"
    done

    # Backup each source
    local backup_failed=0

    for source in "${BACKUP_SOURCES[@]}"; do
        if [ ! -e "$source" ]; then
            log WARNING "Source not found: $source"
            continue
        fi

        local source_name=$(echo "$source" | sed 's/\//_/g')
        local archive_name="${source_name}.tar"

        if [ $COMPRESS -eq 1 ]; then
            archive_name="${archive_name}.gz"
            tar_opts="$tar_opts -z"
        fi

        log INFO "Backing up $source..."

        if tar $tar_opts -cf "$backup_dir/$archive_name" $exclude_opts "$source" 2>/dev/null; then
            local size=$(du -sh "$backup_dir/$archive_name" | cut -f1)
            log SUCCESS "Backed up $source ($size)"
        else
            log ERROR "Failed to backup $source"
            backup_failed=1
        fi
    done

    # Create backup manifest
    cat > "$backup_dir/manifest.txt" << EOF
Backup Manifest
===============

Date: $(date '+%Y-%m-%d %H:%M:%S')
Type: $BACKUP_TYPE
Hostname: $(hostname)
Sources: ${BACKUP_SOURCES[@]}

Archives:
EOF

    ls -lh "$backup_dir" | tail -n +2 >> "$backup_dir/manifest.txt"

    # Calculate total size
    local total_size=$(du -sh "$backup_dir" | cut -f1)
    echo "" >> "$backup_dir/manifest.txt"
    echo "Total Size: $total_size" >> "$backup_dir/manifest.txt"

    log SUCCESS "Backup completed: $backup_dir ($total_size)"

    # Create symlink to latest backup
    ln -snf "$backup_dir" "$BACKUP_DEST/latest"

    return $backup_failed
}

rotate_backups() {
    log INFO "Rotating backups (keeping last $ROTATION_COUNT)..."

    # Find backup directories
    local backups=($(ls -1dt "$BACKUP_DEST"/backup_* 2>/dev/null))
    local backup_count=${#backups[@]}

    if [ $backup_count -le $ROTATION_COUNT ]; then
        log INFO "No backups to rotate (found $backup_count, keeping $ROTATION_COUNT)"
        return 0
    fi

    # Remove old backups
    local to_remove=$((backup_count - ROTATION_COUNT))
    log INFO "Removing $to_remove old backup(s)..."

    for ((i=ROTATION_COUNT; i<backup_count; i++)); do
        log INFO "Removing: ${backups[$i]}"
        rm -rf "${backups[$i]}"
    done

    log SUCCESS "Backup rotation completed"
}

generate_report() {
    local report_file="$BACKUP_DEST/backup_report.txt"

    cat > "$report_file" << EOF
AutomationOS Backup Report
==========================

Generated: $(date '+%Y-%m-%d %H:%M:%S')

Recent Backups:
EOF

    # List recent backups
    ls -lth "$BACKUP_DEST"/backup_* 2>/dev/null | head -n $ROTATION_COUNT >> "$report_file"

    cat >> "$report_file" << EOF

Disk Usage:
EOF

    du -sh "$BACKUP_DEST" >> "$report_file"
    df -h "$BACKUP_DEST" >> "$report_file"

    cat >> "$report_file" << EOF

Configuration:
  Backup Type: $BACKUP_TYPE
  Destination: $BACKUP_DEST
  Rotation: $ROTATION_COUNT backups
  Compression: $([ $COMPRESS -eq 1 ] && echo "Enabled" || echo "Disabled")

EOF

    log INFO "Report generated: $report_file"
}

#=============================================================================
# Main
#=============================================================================

main() {
    echo ""
    echo "═══════════════════════════════════════════"
    echo "  AutomationOS System Backup"
    echo "═══════════════════════════════════════════"
    echo ""

    # Parse command line arguments
    parse_args "$@"

    # Check prerequisites
    check_prerequisites

    # Create backup
    create_backup
    local backup_status=$?

    # Rotate old backups
    rotate_backups

    # Generate report
    generate_report

    # Summary
    echo ""
    echo "═══════════════════════════════════════════"
    if [ $backup_status -eq 0 ]; then
        log SUCCESS "Backup completed successfully"
        echo "═══════════════════════════════════════════"
        echo ""
        exit 0
    else
        log WARNING "Backup completed with errors"
        echo "═══════════════════════════════════════════"
        echo ""
        exit 1
    fi
}

# Run main function
main "$@"
