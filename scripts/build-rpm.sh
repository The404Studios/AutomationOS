#!/bin/bash
#
# AutomationOS RPM Package Builder
#
# Creates an .rpm package for AutomationOS tools and development files.
#
# Usage:
#   ./scripts/build-rpm.sh [VERSION]
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
RPM_ROOT="$ROOT_DIR/rpmbuild"

echo -e "${BLUE}=========================================${NC}"
echo -e "${BLUE}  Building RPM Package${NC}"
echo -e "${BLUE}=========================================${NC}"
echo ""
echo "Version: $VERSION_NO_V"
echo ""

# Check dependencies
print_section "Checking Dependencies"

for tool in rpmbuild; do
    if command -v $tool &> /dev/null; then
        print_success "$tool found"
    else
        print_error "$tool not found"
        echo "Install with:"
        echo "  Fedora/RHEL: sudo dnf install rpm-build"
        echo "  OpenSUSE: sudo zypper install rpm-build"
        exit 1
    fi
done

# Clean previous build
if [ -d "$RPM_ROOT" ]; then
    rm -rf "$RPM_ROOT"
fi

# Create RPM build tree
print_section "Creating RPM Build Tree"

mkdir -p "$RPM_ROOT"/{BUILD,RPMS,SOURCES,SPECS,SRPMS}
print_success "Build tree created"

# Create source tarball
print_section "Creating Source Tarball"

TARBALL_NAME="automationos-${VERSION_NO_V}.tar.gz"
TARBALL_PATH="$RPM_ROOT/SOURCES/$TARBALL_NAME"

tar -czf "$TARBALL_PATH" \
    --transform="s,^,$ROOT_DIR/automationos-${VERSION_NO_V}/," \
    -C "$ROOT_DIR" \
    --exclude="rpmbuild" \
    --exclude="build" \
    --exclude="release" \
    --exclude=".git" \
    --exclude="*.o" \
    --exclude="*.elf" \
    scripts/ docs/ README.md

print_success "Source tarball created: $TARBALL_NAME"

# Create spec file
print_section "Generating RPM Spec File"

SPEC_FILE="$RPM_ROOT/SPECS/automationos.spec"

cat > "$SPEC_FILE" << EOF
Name:           automationos
Version:        ${VERSION_NO_V}
Release:        1%{?dist}
Summary:        AutomationOS development tools and runtime

License:        MIT
URL:            https://github.com/your-org/automationos
Source0:        %{name}-%{version}.tar.gz

BuildArch:      x86_64
BuildRequires:  make
BuildRequires:  python3
Requires:       qemu-system-x86
Requires:       nasm
Requires:       python3
Requires:       make
Requires:       xorriso

%description
AutomationOS is a modern operating system designed for automation
and embedded systems. This package includes development tools,
runtime utilities, and documentation.

Features:
 - Build scripts and tools
 - QEMU integration
 - Development utilities
 - Documentation and examples

%prep
%setup -q

%build
# Nothing to build (scripts only)

%install
rm -rf \$RPM_BUILD_ROOT

# Create directories
install -d \$RPM_BUILD_ROOT%{_bindir}
install -d \$RPM_BUILD_ROOT%{_libdir}/automationos
install -d \$RPM_BUILD_ROOT%{_datadir}/automationos
install -d \$RPM_BUILD_ROOT%{_docdir}/automationos
install -d \$RPM_BUILD_ROOT%{_mandir}/man1

# Install scripts
install -m 755 scripts/run-qemu.sh \$RPM_BUILD_ROOT%{_libdir}/automationos/
install -m 755 scripts/build-iso.py \$RPM_BUILD_ROOT%{_libdir}/automationos/

# Create wrapper scripts
cat > \$RPM_BUILD_ROOT%{_bindir}/automationos-build << 'BUILDEOF'
#!/bin/bash
set -e
if [ -f Makefile ] && grep -q "AutomationOS" Makefile; then
    make all
else
    echo "Error: Not in AutomationOS source directory"
    exit 1
fi
BUILDEOF

cat > \$RPM_BUILD_ROOT%{_bindir}/automationos-qemu << 'QEMUEOF'
#!/bin/bash
exec %{_libdir}/automationos/run-qemu.sh "\$@"
QEMUEOF

chmod 755 \$RPM_BUILD_ROOT%{_bindir}/automationos-build
chmod 755 \$RPM_BUILD_ROOT%{_bindir}/automationos-qemu

# Install documentation
install -m 644 README.md \$RPM_BUILD_ROOT%{_docdir}/automationos/

# Install man page
cat > \$RPM_BUILD_ROOT%{_mandir}/man1/automationos.1 << 'MANEOF'
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
\\fBautomationos-build\\fR builds the operating system from source.
.PP
\\fBautomationos-qemu\\fR runs AutomationOS in QEMU emulator.
.SH OPTIONS
.TP
.B \\-\\-help
Display help message
.TP
.B \\-\\-debug
Start in debug mode with GDB server
.SH EXAMPLES
.TP
Build AutomationOS:
.B automationos-build
.TP
Run in QEMU:
.B automationos-qemu
.SH AUTHOR
AutomationOS Team
.SH BUGS
Report bugs to: https://github.com/your-org/automationos/issues
MANEOF

gzip \$RPM_BUILD_ROOT%{_mandir}/man1/automationos.1

%files
%license LICENSE
%doc README.md
%{_bindir}/automationos-build
%{_bindir}/automationos-qemu
%{_libdir}/automationos/
%{_datadir}/automationos/
%{_docdir}/automationos/
%{_mandir}/man1/automationos.1.gz

%changelog
* $(date "+%a %b %d %Y") AutomationOS Team <contact@automationos.org> - ${VERSION_NO_V}-1
- Release ${VERSION_NO_V}
- Initial RPM package

EOF

print_success "Spec file created"

# Build RPM
print_section "Building RPM Package"

rpmbuild --define "_topdir $RPM_ROOT" -ba "$SPEC_FILE"

# Find generated RPM
RPM_FILE=$(find "$RPM_ROOT/RPMS" -name "*.rpm" | head -n1)

if [ -f "$RPM_FILE" ]; then
    size=$(du -h "$RPM_FILE" | cut -f1)
    print_success "RPM built: $(basename "$RPM_FILE") ($size)"

    # Move to release directory
    mkdir -p "$ROOT_DIR/release"
    cp "$RPM_FILE" "$ROOT_DIR/release/"
    print_success "Package moved to release/"

    # Verify package
    print_section "Verifying Package"
    rpm -qilp "$ROOT_DIR/release/$(basename "$RPM_FILE")"

    echo ""
    echo -e "${GREEN}=========================================${NC}"
    echo -e "${GREEN}  RPM Package Built Successfully${NC}"
    echo -e "${GREEN}=========================================${NC}"
    echo ""
    echo "Package: release/$(basename "$RPM_FILE")"
    echo ""
    echo "Install with:"
    echo "  sudo dnf install release/$(basename "$RPM_FILE")  # Fedora"
    echo "  sudo yum install release/$(basename "$RPM_FILE")  # RHEL/CentOS"
    echo "  sudo zypper install release/$(basename "$RPM_FILE")  # OpenSUSE"
    echo ""
else
    print_error "RPM build failed"
    exit 1
fi
