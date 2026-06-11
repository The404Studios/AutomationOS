#!/usr/bin/autoshell
#
# AutomationOS Application Installer
#
# Install, remove, and manage applications from packages
# Supports multiple package formats and repositories
#
# Usage: install-app.sh <command> [options] [package]
#   install <pkg>     Install package
#   remove <pkg>      Remove package
#   update <pkg>      Update package
#   search <term>     Search for packages
#   list              List installed packages
#   info <pkg>        Show package information
#   verify <pkg>      Verify package integrity
#

# Configuration
PACKAGE_DB="/var/lib/packages"
CACHE_DIR="/var/cache/packages"
REPO_URL="https://packages.automationos.org"
LOG_FILE="/var/log/install-app.log"

# Colors
COLOR_RESET="\033[0m"
COLOR_RED="\033[31m"
COLOR_GREEN="\033[32m"
COLOR_YELLOW="\033[33m"
COLOR_BLUE="\033[34m"
COLOR_BOLD="\033[1m"

#=============================================================================
# Functions
#=============================================================================

log() {
    local level="$1"
    local message="$2"
    local timestamp=$(date '+%Y-%m-%d %H:%M:%S')

    echo "$timestamp [$level] $message" >> "$LOG_FILE"

    case "$level" in
        INFO)
            echo -e "${COLOR_BLUE}[INFO]${COLOR_RESET} $message"
            ;;
        SUCCESS)
            echo -e "${COLOR_GREEN}[✓]${COLOR_RESET} $message"
            ;;
        WARNING)
            echo -e "${COLOR_YELLOW}[!]${COLOR_RESET} $message"
            ;;
        ERROR)
            echo -e "${COLOR_RED}[✗]${COLOR_RESET} $message"
            ;;
    esac
}

show_help() {
    cat << EOF
AutomationOS Application Installer

Usage: install-app.sh <command> [options] [package]

Commands:
  install <pkg>     Install a package
  remove <pkg>      Remove a package
  update <pkg>      Update a package
  search <term>     Search for packages
  list              List installed packages
  info <pkg>        Show package information
  verify <pkg>      Verify package integrity
  clean             Clean package cache
  help              Show this help

Examples:
  install-app.sh install nginx           # Install nginx
  install-app.sh remove nginx            # Remove nginx
  install-app.sh search web              # Search for web-related packages
  install-app.sh list                    # List all installed packages
  install-app.sh info nginx              # Show nginx package info

EOF
    exit 0
}

init_package_db() {
    mkdir -p "$PACKAGE_DB" "$CACHE_DIR" 2>/dev/null

    if [ ! -f "$PACKAGE_DB/installed.db" ]; then
        touch "$PACKAGE_DB/installed.db"
    fi
}

is_installed() {
    local package="$1"

    if [ -f "$PACKAGE_DB/installed.db" ]; then
        grep -q "^$package:" "$PACKAGE_DB/installed.db"
        return $?
    fi

    return 1
}

install_package() {
    local package="$1"

    log INFO "Installing package: $package"

    # Check if already installed
    if is_installed "$package"; then
        log WARNING "Package already installed: $package"
        echo "Use 'update $package' to upgrade"
        return 1
    fi

    # Check dependencies
    log INFO "Checking dependencies..."
    local deps=$(get_dependencies "$package")

    if [ -n "$deps" ]; then
        log INFO "Dependencies: $deps"

        for dep in $deps; do
            if ! is_installed "$dep"; then
                log INFO "Installing dependency: $dep"
                install_package "$dep" || {
                    log ERROR "Failed to install dependency: $dep"
                    return 1
                }
            fi
        done
    fi

    # Download package
    log INFO "Downloading $package..."

    local package_file="$CACHE_DIR/${package}.tar.gz"

    # Simulate download (in real implementation, use curl/wget)
    if ! download_package "$package" "$package_file"; then
        log ERROR "Failed to download package: $package"
        return 1
    fi

    # Verify package
    log INFO "Verifying package integrity..."

    if ! verify_package "$package_file"; then
        log WARNING "Package verification failed"
        read -p "Continue anyway? (y/n) " -n 1 -r
        echo
        if [[ ! $REPLY =~ ^[Yy]$ ]]; then
            return 1
        fi
    fi

    # Extract package
    log INFO "Extracting package..."

    if ! extract_package "$package_file" "$package"; then
        log ERROR "Failed to extract package"
        return 1
    fi

    # Run installation script
    local install_script="$PACKAGE_DB/$package/install.sh"

    if [ -f "$install_script" ]; then
        log INFO "Running installation script..."

        if bash "$install_script"; then
            log SUCCESS "Installation script completed"
        else
            log ERROR "Installation script failed"
            return 1
        fi
    fi

    # Register package
    local version=$(get_package_version "$package")
    local timestamp=$(date '+%Y-%m-%d %H:%M:%S')

    echo "$package:$version:$timestamp" >> "$PACKAGE_DB/installed.db"

    log SUCCESS "Package installed: $package $version"

    return 0
}

remove_package() {
    local package="$1"

    log INFO "Removing package: $package"

    # Check if installed
    if ! is_installed "$package"; then
        log ERROR "Package not installed: $package"
        return 1
    fi

    # Check reverse dependencies
    local rdeps=$(get_reverse_dependencies "$package")

    if [ -n "$rdeps" ]; then
        log WARNING "The following packages depend on $package:"
        echo "$rdeps"
        read -p "Continue removal? (y/n) " -n 1 -r
        echo
        if [[ ! $REPLY =~ ^[Yy]$ ]]; then
            return 1
        fi
    fi

    # Run uninstall script
    local uninstall_script="$PACKAGE_DB/$package/uninstall.sh"

    if [ -f "$uninstall_script" ]; then
        log INFO "Running uninstallation script..."

        if bash "$uninstall_script"; then
            log SUCCESS "Uninstallation script completed"
        else
            log WARNING "Uninstallation script failed"
        fi
    fi

    # Remove package files
    rm -rf "$PACKAGE_DB/$package"

    # Unregister package
    sed -i "/^$package:/d" "$PACKAGE_DB/installed.db"

    log SUCCESS "Package removed: $package"

    return 0
}

update_package() {
    local package="$1"

    log INFO "Updating package: $package"

    if ! is_installed "$package"; then
        log ERROR "Package not installed: $package"
        return 1
    fi

    local current_version=$(get_package_version "$package")
    local latest_version=$(get_latest_version "$package")

    log INFO "Current version: $current_version"
    log INFO "Latest version: $latest_version"

    if [ "$current_version" = "$latest_version" ]; then
        log INFO "Package is already up to date"
        return 0
    fi

    log INFO "Updating $package from $current_version to $latest_version"

    # Remove old version
    remove_package "$package" || return 1

    # Install new version
    install_package "$package" || return 1

    log SUCCESS "Package updated: $package"

    return 0
}

search_packages() {
    local term="$1"

    log INFO "Searching for: $term"

    echo ""
    echo "Available packages:"
    echo "─────────────────────────────────────────────────"

    # Simulate package search
    local results=$(
        cat << EOF
nginx:1.21.0:Fast HTTP server and reverse proxy
python:3.10.2:Python programming language
gcc:11.2.0:GNU Compiler Collection
git:2.35.0:Distributed version control system
vim:8.2:Vi IMproved text editor
docker:20.10.12:Container platform
nodejs:16.14.0:JavaScript runtime
postgresql:14.2:Advanced relational database
redis:6.2.6:In-memory data structure store
automaton:1.0.0:AutomationOS system utilities
EOF
    )

    echo "$results" | grep -i "$term" | while IFS=: read name version desc; do
        local installed=""
        if is_installed "$name"; then
            installed=" ${COLOR_GREEN}[installed]${COLOR_RESET}"
        fi

        echo -e "  ${COLOR_BOLD}$name${COLOR_RESET} ($version)$installed"
        echo "    $desc"
        echo ""
    done

    echo "─────────────────────────────────────────────────"
}

list_packages() {
    log INFO "Listing installed packages"

    if [ ! -f "$PACKAGE_DB/installed.db" ] || [ ! -s "$PACKAGE_DB/installed.db" ]; then
        echo "No packages installed"
        return 0
    fi

    echo ""
    echo "Installed packages:"
    echo "─────────────────────────────────────────────────"

    while IFS=: read name version timestamp; do
        echo -e "  ${COLOR_BOLD}$name${COLOR_RESET} $version"
        echo "    Installed: $timestamp"
        echo ""
    done < "$PACKAGE_DB/installed.db"

    local count=$(wc -l < "$PACKAGE_DB/installed.db")
    echo "─────────────────────────────────────────────────"
    echo "Total: $count package(s)"
    echo ""
}

show_info() {
    local package="$1"

    echo ""
    echo "Package Information: $package"
    echo "═════════════════════════════════════════════════"

    if is_installed "$package"; then
        local info=$(grep "^$package:" "$PACKAGE_DB/installed.db")
        local version=$(echo "$info" | cut -d: -f2)
        local timestamp=$(echo "$info" | cut -d: -f3)

        echo "Status:      ${COLOR_GREEN}Installed${COLOR_RESET}"
        echo "Version:     $version"
        echo "Installed:   $timestamp"
    else
        echo "Status:      Not installed"
        local version=$(get_latest_version "$package")
        echo "Version:     $version (available)"
    fi

    echo ""
    echo "Description:"
    get_package_description "$package" | sed 's/^/  /'

    echo ""
    echo "Dependencies:"
    local deps=$(get_dependencies "$package")
    if [ -n "$deps" ]; then
        for dep in $deps; do
            echo "  - $dep"
        done
    else
        echo "  None"
    fi

    echo ""
    echo "Size:        $(get_package_size "$package")"
    echo "Homepage:    $(get_package_homepage "$package")"
    echo ""
}

verify_package() {
    local package_file="$1"

    # Simple verification (in real implementation, check checksums)
    if [ -f "$package_file" ]; then
        return 0
    else
        return 1
    fi
}

clean_cache() {
    log INFO "Cleaning package cache..."

    local size_before=$(du -sh "$CACHE_DIR" 2>/dev/null | cut -f1)

    rm -rf "$CACHE_DIR"/*

    local size_after=$(du -sh "$CACHE_DIR" 2>/dev/null | cut -f1)

    log SUCCESS "Cache cleaned (freed: $size_before)"
}

#=============================================================================
# Helper functions (stubs for demonstration)
#=============================================================================

download_package() {
    local package="$1"
    local dest="$2"

    # Simulate download
    echo "Simulating download of $package..." > "$dest"
    return 0
}

extract_package() {
    local package_file="$1"
    local package="$2"

    mkdir -p "$PACKAGE_DB/$package"
    return 0
}

get_dependencies() {
    local package="$1"

    # Stub: return empty for most packages
    case "$package" in
        nginx) echo "openssl pcre" ;;
        postgresql) echo "openssl readline" ;;
        *) echo "" ;;
    esac
}

get_reverse_dependencies() {
    local package="$1"
    echo ""  # Stub
}

get_package_version() {
    local package="$1"

    if is_installed "$package"; then
        grep "^$package:" "$PACKAGE_DB/installed.db" | cut -d: -f2
    else
        echo "1.0.0"  # Stub
    fi
}

get_latest_version() {
    echo "1.0.1"  # Stub
}

get_package_description() {
    local package="$1"
    echo "This is the $package package."
}

get_package_size() {
    echo "5.2 MB"  # Stub
}

get_package_homepage() {
    echo "https://example.com"  # Stub
}

#=============================================================================
# Main
#=============================================================================

main() {
    if [ $# -lt 1 ]; then
        show_help
    fi

    init_package_db

    local command="$1"
    shift

    case "$command" in
        install)
            if [ $# -lt 1 ]; then
                log ERROR "Missing package name"
                echo "Usage: install-app.sh install <package>"
                exit 1
            fi
            install_package "$1"
            ;;

        remove)
            if [ $# -lt 1 ]; then
                log ERROR "Missing package name"
                echo "Usage: install-app.sh remove <package>"
                exit 1
            fi
            remove_package "$1"
            ;;

        update)
            if [ $# -lt 1 ]; then
                log ERROR "Missing package name"
                echo "Usage: install-app.sh update <package>"
                exit 1
            fi
            update_package "$1"
            ;;

        search)
            if [ $# -lt 1 ]; then
                log ERROR "Missing search term"
                echo "Usage: install-app.sh search <term>"
                exit 1
            fi
            search_packages "$1"
            ;;

        list)
            list_packages
            ;;

        info)
            if [ $# -lt 1 ]; then
                log ERROR "Missing package name"
                echo "Usage: install-app.sh info <package>"
                exit 1
            fi
            show_info "$1"
            ;;

        verify)
            if [ $# -lt 1 ]; then
                log ERROR "Missing package name"
                echo "Usage: install-app.sh verify <package>"
                exit 1
            fi
            verify_package "$PACKAGE_DB/$1"
            ;;

        clean)
            clean_cache
            ;;

        help|--help|-h)
            show_help
            ;;

        *)
            log ERROR "Unknown command: $command"
            echo "Use 'install-app.sh help' for usage information"
            exit 1
            ;;
    esac

    exit $?
}

# Run main
main "$@"
