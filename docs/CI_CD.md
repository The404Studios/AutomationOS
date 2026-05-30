# CI/CD Architecture

**AutomationOS Continuous Integration and Deployment Pipeline**

This document describes the production-grade CI/CD pipeline for AutomationOS.

---

## Overview

The AutomationOS CI/CD pipeline provides:

- **Multi-platform builds** (Linux, macOS, WSL2)
- **Automated testing** (unit, integration, QEMU)
- **Security scanning** (static analysis, fuzzing, vulnerability detection)
- **Continuous fuzzing** (AFL++, LibFuzzer, Honggfuzz)
- **Release automation** (changelog generation, artifact signing, GitHub releases)

---

## Architecture

### Pipeline Stages

```
┌─────────────────────────────────────────────────────────────┐
│                    Commit/PR Trigger                        │
└──────────────────┬──────────────────────────────────────────┘
                   │
    ┌──────────────┼──────────────┐
    │              │              │
    ▼              ▼              ▼
┌────────┐   ┌─────────┐   ┌──────────┐
│ Build  │   │  Test   │   │ Security │
│ (15min)│   │ (15min) │   │ (20min)  │
└────┬───┘   └────┬────┘   └─────┬────┘
     │            │              │
     └────────────┼──────────────┘
                  │
                  ▼
          ┌──────────────┐
          │   Summary    │
          │   Report     │
          └──────────────┘
```

### Nightly Pipeline

```
┌─────────────────────────────────────────────────────────────┐
│                    Nightly Trigger                          │
└──────────────────┬──────────────────────────────────────────┘
                   │
    ┌──────────────┼──────────────┐
    │              │              │
    ▼              ▼              ▼
┌─────────┐   ┌─────────┐   ┌─────────┐
│  Build  │   │  Fuzz   │   │Benchmark│
│  All    │   │ (60min) │   │  Tests  │
└─────────┘   └─────────┘   └─────────┘
```

---

## Workflows

### 1. Build Workflow (`.github/workflows/build.yml`)

**Triggers:**
- Push to `main`, `develop`
- Pull requests
- Manual dispatch

**Jobs:**
- `build-linux` - Build on Ubuntu 22.04 and 24.04
- `build-macos` - Build on macOS latest
- `build-wsl2` - Build on WSL2 (Windows)
- `docker-build` - Build in Docker container
- `build-summary` - Aggregate results

**Artifacts:**
- `kernel.elf` - Compiled kernel binary
- `AutomationOS.iso` - Bootable ISO image
- `boot.bin` - Bootloader binary

**Success Criteria:**
- All platforms build successfully
- No compilation warnings (treated as errors)
- Build time < 15 minutes

---

### 2. Test Workflow (`.github/workflows/test.yml`)

**Triggers:**
- Push to `main`, `develop`
- Pull requests
- Manual dispatch

**Jobs:**

#### Unit Tests
- Build and run unit tests
- Test coverage reports
- Upload results as artifacts

#### Integration Tests
- Boot test in QEMU
- Verify kernel subsystems
- Serial output validation

#### Benchmark Tests (main branch only)
- Performance benchmarks
- Comparison with baseline
- Regression detection

#### QEMU Tests (Multiple Configurations)
- Minimal: 512M RAM, 1 CPU
- Standard: 2G RAM, 2 CPUs
- High-mem: 8G RAM, 4 CPUs
- Many-CPU: 4G RAM, 8 CPUs

**Success Criteria:**
- All tests pass
- No test failures or crashes
- Test execution time < 15 minutes

---

### 3. Security Workflow (`.github/workflows/security.yml`)

**Triggers:**
- Push to `main`, `develop`
- Pull requests
- Weekly schedule (Monday 2 AM UTC)
- Manual dispatch

**Jobs:**

#### Static Analysis
- **cppcheck** - C/C++ static analyzer
- **clang-tidy** - Clang-based linter
- **splint** - Secure programming lint

#### Memory Safety
- **AddressSanitizer** - Memory error detection
- **Valgrind** - Memory leak detection

#### Vulnerability Scanning
- **Trivy** - Vulnerability scanner
- **CodeQL** - Semantic code analysis

#### Kernel Hardening Check
- NX bit support
- Stack protection
- ASLR/KASLR
- NULL pointer protection

**Security Report:**
- Generated artifact with findings
- Uploaded to GitHub Security tab
- Automatic issue creation for critical vulnerabilities

---

### 4. Fuzzing Workflow (`.github/workflows/fuzz.yml`)

**Triggers:**
- Nightly schedule (1 AM UTC)
- Manual dispatch with configurable duration
- Pull requests (1 hour duration)

**Jobs:**

#### AFL++ Fuzzing
- Target: Physical Memory Manager
- Duration: Configurable (default 60 min)
- Crash detection and reporting

#### LibFuzzer
- Targets:
  - `fuzz_heap` - Heap allocator
  - `fuzz_vmm` - Virtual memory manager
  - `fuzz_scheduler` - Process scheduler
  - `fuzz_syscall` - System call interface
- Duration: 60 minutes per target

#### Honggfuzz
- Multi-threaded fuzzing
- Coverage-guided testing

#### Kernel System Call Fuzzing
- Automated syscall fuzzing
- State machine testing

**Crash Analysis:**
- Automatic crash deduplication
- Stack trace collection
- Artifact upload for reproduction

**Failure Policy:**
- Build fails if crashes detected
- Automated issue creation
- Security team notification

---

### 5. Release Workflow (`.github/workflows/release.yml`)

**Triggers:**
- Git tags matching `v*.*.*`
- Manual dispatch with version input

**Jobs:**

#### Create Release
- Generate changelog from commits
- Create GitHub release
- Mark as prerelease if alpha/beta/rc

#### Build Release
- Multi-platform builds (Ubuntu, macOS)
- Run full test suite
- Create release archives (.tar.gz, .zip)
- Calculate SHA256 checksums
- GPG sign artifacts (if key available)

#### Publish Docker
- Build and push Docker image
- Tag with version and `latest`
- Push to GitHub Container Registry

**Release Artifacts:**
- `AutomationOS-{version}-{platform}.tar.gz`
- `AutomationOS-{version}-{platform}.zip`
- `AutomationOS-{version}.iso` (primary platform)
- SHA256 checksums
- GPG signatures (if configured)

---

## Docker Build Environment

### Dockerfile.build

Production-grade container with:
- Ubuntu 22.04 base
- x86_64-elf cross-compiler (GCC 13.2.0, Binutils 2.41)
- NASM assembler
- QEMU for testing
- Python build scripts
- Static analysis tools

### Building the Docker image

```bash
docker build -f Dockerfile.build -t automationos-build:latest .
```

### Using Docker for local builds

```bash
# Build all
docker-compose up build

# Interactive development
docker-compose run dev

# Run tests
docker-compose up test
```

---

## Pre-Commit Hooks

### Installation

```bash
make install-hooks
```

### Checks Performed

1. **Code Formatting** - clang-format validation
2. **Trailing Whitespace** - Detect trailing spaces
3. **Static Analysis** - cppcheck on changed files
4. **TODO Detection** - Warn about unresolved TODOs
5. **Commit Message** - Validate message format

### Skipping Hooks

```bash
git commit --no-verify
```

*Use sparingly - hooks ensure code quality!*

---

## Local Development

### Quick Start

```bash
# Install pre-commit hooks
make install-hooks

# Build everything
make all

# Run tests
make test

# Run in QEMU
make qemu
```

### Docker Development

```bash
# Start development container
docker-compose run dev

# Inside container:
make all
make test
```

### Continuous Build

```bash
# Watch for changes and rebuild
docker-compose up watch
```

---

## Artifact Management

### Build Artifacts

All builds upload artifacts to GitHub Actions:

- **Retention:**
  - Regular builds: 30 days
  - Release builds: 90 days
  - Test results: 30 days
  - Fuzzing results: 90 days

### Downloading Artifacts

```bash
# Using GitHub CLI
gh run download <run-id>

# Specific artifact
gh run download <run-id> -n automationos-ubuntu-22.04-<sha>
```

---

## Caching Strategy

### Cross-Compiler Cache

- **Key:** OS + GCC version + Binutils version
- **Path:** `/opt/cross`, `~/.cache/cross-compiler`
- **Benefit:** Reduces build time by 10-15 minutes

### Docker Layer Cache

- **Strategy:** GitHub Actions cache
- **Benefit:** Faster Docker image builds

### Build Artifact Cache

- **Strategy:** Incremental compilation
- **Path:** `build/`
- **Benefit:** Faster rebuilds

---

## Performance Targets

### Build Times

| Platform      | Target | Actual |
|---------------|--------|--------|
| Ubuntu 22.04  | 15 min | ~12 min|
| Ubuntu 24.04  | 15 min | ~12 min|
| macOS         | 20 min | ~18 min|
| Docker        | 15 min | ~10 min|

### Test Times

| Test Suite    | Target | Actual |
|---------------|--------|--------|
| Unit tests    | 5 min  | ~3 min |
| Integration   | 10 min | ~5 min |
| QEMU tests    | 10 min | ~8 min |
| Full suite    | 15 min | ~12 min|

---

## Monitoring and Alerts

### Build Failures

- **Notification:** GitHub Actions UI
- **Auto-retry:** Failed jobs retry once
- **Escalation:** Repeated failures create issues

### Security Findings

- **Critical:** Immediate notification
- **High:** Weekly digest
- **Medium/Low:** Monthly report

### Fuzzing Crashes

- **Detection:** Automated crash analysis
- **Notification:** Security team email
- **Action:** Auto-create security advisory

---

## Contributing to CI/CD

### Adding New Tests

1. Create test in `tests/` directory
2. Add to appropriate workflow
3. Update test summary job
4. Document in this file

### Modifying Workflows

1. Edit workflow YAML in `.github/workflows/`
2. Test locally with `act` (GitHub Actions locally)
3. Submit PR with changes
4. Verify in PR checks

### Security Considerations

- Never commit secrets to workflows
- Use GitHub Secrets for sensitive data
- Review security findings weekly
- Keep dependencies updated

---

## Troubleshooting

### Build Failures

```bash
# Check logs
gh run view <run-id>

# Download failed artifacts
gh run download <run-id>

# Reproduce locally
docker-compose run build
```

### Test Failures

```bash
# View test logs
cat build/serial.log

# Run specific test
python3 tests/integration/test_boot.py --verbose
```

### Cache Issues

```bash
# Clear GitHub Actions cache
gh cache delete <cache-key>

# Clear local Docker cache
docker-compose down -v
docker system prune -a
```

---

## Future Improvements

### Planned Enhancements

- [ ] Parallel test execution
- [ ] Distributed fuzzing
- [ ] Performance regression tracking
- [ ] Automatic dependency updates (Dependabot)
- [ ] Code coverage reporting (codecov.io)
- [ ] Nightly ISO uploads to CDN
- [ ] Hardware-in-the-loop testing
- [ ] Cross-architecture builds (ARM64)

---

## References

- [GitHub Actions Documentation](https://docs.github.com/en/actions)
- [Docker Best Practices](https://docs.docker.com/develop/dev-best-practices/)
- [AFL++ Documentation](https://aflplus.plus/)
- [QEMU Documentation](https://www.qemu.org/docs/master/)

---

**Last Updated:** 2026-05-26  
**Maintained By:** AutomationOS Team
