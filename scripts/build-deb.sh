#!/bin/bash
#
# AutomationOS Debian Package Builder
#
# Creates a .deb package for AutomationOS tools and development files.
#
# Usage:
#   ./scripts/build-deb.sh [VERSION]
#

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

print_section() {
    echo ""
    echo -e "${GREEN}>>> $1${NC}"
}

print_success() {
    echo -e "${GREEN}✓${NC} $1"
}

print_error() {
    echo -e "${RED}✗${NC} $1"
}

# Get version
VERSION="${1:-1.0.0}"
VERSION_NO_V="${VERSION#v}"

# Directories
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
PACKAGE_DIR="$ROOT_DIR/package-deb"
DEB_DIR="$PACKAGE_DIR/automationos_${VERSION_NO_V}_amd64"

echo -e "${BLUE}=========================================${NC}"
echo -e "${BLUE}  Building Debian Package${NC}"
echo -e "${BLUE}=========================================${NC}"
echo ""
echo "Version: $VERSION_NO_V"
echo ""

# Check dependencies
print_section "Checking Dependencies"

for tool in dpkg-deb fakeroot; do
    if command -v $tool &> /dev/null; then
        print_success "$tool found"
    else
        print_error "$tool not found"
        echo "Install with: sudo apt install dpkg-dev fakeroot"
        exit 1
    fi
done

# Clean previous package
if [ -d "$PACKAGE_DIR" ]; then
    rm -rf "$PACKAGE_DIR"
fi

# Create package structure
print_section "Creating Package Structure"

mkdir -p "$DEB_DIR/DEBIAN"
mkdir -p "$DEB_DIR/usr/bin"
mkdir -p "$DEB_DIR/usr/lib/automationos"
mkdir -p "$DEB_DIR/usr/share/automationos"
mkdir -p "$DEB_DIR/usr/share/doc/automationos"
mkdir -p "$DEB_DIR/usr/share/man/man1"

print_success "Package directory created"

# Create control file
print_section "Generating Control Files"

cat > "$DEB_DIR/DEBIAN/control" << EOF
Package: automationos
Version: $VERSION_NO_V
Section: devel
Priority: optional
Architecture: amd64
Depends: qemu-system-x86, nasm, python3, make, xorriso
Recommends: gcc-x86-64-elf, binutils-x86-64-elf
Maintainer: AutomationOS Team <contact@automationos.org>
Description: AutomationOS development tools and runtime
 AutomationOS is a modern operating system designed for automation
 and embedded systems. This package includes development tools,
 runtime utilities, and documentation.
 .
 Features:
  - Build scripts and tools
  - QEMU integration
  - Development utilities
  - Documentation and examples
Homepage: https://github.com/your-org/automationos
EOF

print_success "Control file created"

# Create postinst script
cat > "$DEB_DIR/DEBIAN/postinst" << 'EOF'
#!/bin/bash
set -e

echo "Setting up AutomationOS..."

# Create symlinks
if [ ! -e /usr/bin/automationos-build ]; then
    ln -s /usr/lib/automationos/build.sh /usr/bin/automationos-build
fi

if [ ! -e /usr/bin/automationos-qemu ]; then
    ln -s /usr/lib/automationos/run-qemu.sh /usr/bin/automationos-qemu
fi

echo "AutomationOS installed successfully!"
echo ""
echo "Quick start:"
echo "  automationos-build    # Build AutomationOS"
echo "  automationos-qemu     # Run in QEMU"
echo ""

exit 0
EOF

chmod 755 "$DEB_DIR/DEBIAN/postinst"
print_success "Post-install script created"

# Create prerm script
cat > "$DEB_DIR/DEBIAN/prerm" << 'EOF'
#!/bin/bash
set -e

# Remove symlinks
rm -f /usr/bin/automationos-build
rm -f /usr/bin/automationos-qemu

exit 0
EOF

chmod 755 "$DEB_DIR/DEBIAN/prerm"
print_success "Pre-remove script created"

# Copy files
print_section "Copying Files"

# Build scripts
cp "$ROOT_DIR/scripts/run-qemu.sh" "$DEB_DIR/usr/lib/automationos/"
cp "$ROOT_DIR/scripts/build-iso.py" "$DEB_DIR/usr/lib/automationos/"

# Create wrapper script
cat > "$DEB_DIR/usr/lib/automationos/build.sh" << 'EOF'
#!/bin/bash
# AutomationOS build wrapper
set -e

if [ -f Makefile ] && grep -q "AutomationOS" Makefile; then
    make all
else
    echo "Error: Not in AutomationOS source directory"
    exit 1
fi
EOF

chmod 755 "$DEB_DIR/usr/lib/automationos/build.sh"

print_success "Scripts copied"

# Copy documentation
if [ -f "$ROOT_DIR/README.md" ]; then
    cp "$ROOT_DIR/README.md" "$DEB_DIR/usr/share/doc/automationos/"
fi

if [ -f "$ROOT_DIR/CHANGELOG.md" ]; then
    cp "$ROOT_DIR/CHANGELOG.md" "$DEB_DIR/usr/share/doc/automationos/"
fi

# Create copyright file
cat > "$DEB_DIR/usr/share/doc/automationos/copyright" << EOF
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: automationos
Source: https://github.com/your-org/automationos

Files: *
Copyright: $(date +%Y) AutomationOS Team
License: MIT
 [Add full license text here]
EOF

print_success "Documentation copied"

# Create changelog
cat > "$DEB_DIR/usr/share/doc/automationos/changelog.Debian" << EOF
automationos ($VERSION_NO_V) unstable; urgency=medium

  * Release $VERSION_NO_V

 -- AutomationOS Team <contact@automationos.org>  $(date -R)
EOF

gzip -9 "$DEB_DIR/usr/share/doc/automationos/changelog.Debian"

# Create man page
cat > "$DEB_DIR/usr/share/man/man1/automationos.1" << 'EOF'
.TH AUTOMATIONOS 1 "2024" "AutomationOS 1.0" "User Commands"
.SH NAME
automationos \- AutomationOS development tools
.SH SYNOPSIS
.B automationos-build
.br
.B automationos-qemu
[\fIOPTIONS\fR]
.SH DESCRIPTION
AutomationOS is a modern operating system designed for automation
and embedded systems.
.PP
\fBautomationos-build\fR builds the operating system from source.
.PP
\fBautomationos-qemu\fR runs AutomationOS in QEMU emulator.
.SH OPTIONS
.TP
.B \-\-help
Display help message
.TP
.B \-\-debug
Start in debug mode with GDB server
.SH EXAMPLES
.TP
Build AutomationOS:
.B automationos-build
.TP
Run in QEMU:
.B automationos-qemu
.TP
Debug in QEMU:
.B automationos-qemu --debug
.SH SEE ALSO
.BR qemu (1),
.BR make (1)
.SH AUTHOR
AutomationOS Team
.SH BUGS
Report bugs to: https://github.com/your-org/automationos/issues
EOF

gzip -9 "$DEB_DIR/usr/share/man/man1/automationos.1"
print_success "Man page created"

# Set permissions
print_section "Setting Permissions"

find "$DEB_DIR" -type d -exec chmod 755 {} \;
find "$DEB_DIR" -type f -exec chmod 644 {} \;
chmod 755 "$DEB_DIR/usr/lib/automationos"/*.sh
chmod 755 "$DEB_DIR/usr/lib/automationos"/*.py
chmod 755 "$DEB_DIR/DEBIAN/postinst"
chmod 755 "$DEB_DIR/DEBIAN/prerm"

print_success "Permissions set"

# Calculate installed size
INSTALLED_SIZE=$(du -sk "$DEB_DIR" | cut -f1)
echo "Installed-Size: $INSTALLED_SIZE" >> "$DEB_DIR/DEBIAN/control"

# Build package
print_section "Building Package"

cd "$PACKAGE_DIR"
fakeroot dpkg-deb --build "automationos_${VERSION_NO_V}_amd64"

DEB_FILE="automationos_${VERSION_NO_V}_amd64.deb"

if [ -f "$DEB_FILE" ]; then
    size=$(du -h "$DEB_FILE" | cut -f1)
    print_success "Package built: $DEB_FILE ($size)"

    # Move to release directory
    mkdir -p "$ROOT_DIR/release"
    mv "$DEB_FILE" "$ROOT_DIR/release/"
    print_success "Package moved to release/"

    # Verify package
    print_section "Verifying Package"
    dpkg-deb -I "$ROOT_DIR/release/$DEB_FILE"
    dpkg-deb -c "$ROOT_DIR/release/$DEB_FILE"

    echo ""
    echo -e "${GREEN}=========================================${NC}"
    echo -e "${GREEN}  Debian Package Built Successfully${NC}"
    echo -e "${GREEN}=========================================${NC}"
    echo ""
    echo "Package: release/$DEB_FILE"
    echo ""
    echo "Install with:"
    echo "  sudo dpkg -i release/$DEB_FILE"
    echo "  sudo apt-get install -f  # Install dependencies"
    echo ""
else
    print_error "Package build failed"
    exit 1
fi
