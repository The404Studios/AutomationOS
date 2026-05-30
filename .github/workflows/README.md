# GitHub Actions Workflows

This directory contains the CI/CD pipeline configuration for AutomationOS.

## Workflows

### Build Pipeline (`build.yml`)

**Triggers:** Push, Pull Request, Manual

**Purpose:** Multi-platform builds

**Jobs:**
- `build-linux` - Ubuntu 22.04 & 24.04
- `build-macos` - macOS latest
- `build-wsl2` - Windows WSL2
- `docker-build` - Docker container
- `build-summary` - Results aggregation

**Duration:** ~15 minutes

**Artifacts:** `kernel.elf`, `AutomationOS.iso`, `boot.bin`

---

### Test Pipeline (`test.yml`)

**Triggers:** Push, Pull Request, Manual

**Purpose:** Automated testing

**Jobs:**
- `unit-tests` - C unit tests
- `integration-tests` - Boot tests in QEMU
- `benchmark-tests` - Performance benchmarks (main branch only)
- `qemu-tests` - Multiple VM configurations
- `test-summary` - Results aggregation

**Duration:** ~15 minutes

**Success Criteria:** All tests pass, no crashes

---

### Security Scanning (`security.yml`)

**Triggers:** Push, Pull Request, Weekly, Manual

**Purpose:** Security analysis and vulnerability detection

**Jobs:**
- `static-analysis` - cppcheck, clang-tidy, splint
- `memory-safety` - AddressSanitizer, Valgrind
- `vulnerability-scan` - Trivy scanner
- `codeql-analysis` - Semantic code analysis
- `dependency-check` - Dependency auditing
- `kernel-hardening-check` - Security feature validation
- `security-summary` - Results aggregation

**Duration:** ~20 minutes

**Schedule:** Weekly on Monday 2 AM UTC

---

### Fuzzing (`fuzz.yml`)

**Triggers:** Nightly, Pull Request, Manual

**Purpose:** Continuous fuzzing for bug discovery

**Jobs:**
- `afl-fuzzing` - AFL++ coverage-guided fuzzing
- `libfuzzer` - Multiple targets (heap, vmm, scheduler, syscall)
- `honggfuzz` - Multi-threaded fuzzing
- `kernel-fuzzing` - System call fuzzing
- `fuzzing-summary` - Results aggregation

**Duration:** Configurable (default 60 minutes)

**Schedule:** Nightly at 1 AM UTC

**Failure Policy:** Fails if crashes detected

---

### Release (`release.yml`)

**Triggers:** Git tags (`v*.*.*`), Manual

**Purpose:** Automated release creation

**Jobs:**
- `create-release` - Generate changelog, create GitHub release
- `build-release` - Multi-platform release builds
- `publish-docker` - Push Docker image to registry
- `notify` - Release notification

**Artifacts:**
- `AutomationOS-{version}-{platform}.tar.gz`
- `AutomationOS-{version}-{platform}.zip`
- `AutomationOS-{version}.iso`
- SHA256 checksums
- GPG signatures (if configured)

---

### Nightly Build (`nightly.yml`)

**Triggers:** Daily at 2 AM UTC, Manual

**Purpose:** Comprehensive nightly testing

**Jobs:**
- `nightly-build` - Full system build
- `extended-tests` - Memory configurations, stress testing
- `performance-benchmark` - Performance regression tracking
- `nightly-summary` - Results aggregation

**Duration:** ~90 minutes

**Artifacts:** Retained for 7 days

---

## Workflow Dependencies

```
Push/PR
  ├── build.yml (parallel)
  ├── test.yml (parallel)
  └── security.yml (parallel)

Nightly
  ├── nightly.yml
  │   ├── nightly-build
  │   ├── extended-tests
  │   └── performance-benchmark
  └── fuzz.yml

Release (tag)
  └── release.yml
      ├── create-release
      ├── build-release
      └── publish-docker
```

## Status Badges

Add to README.md:

```markdown
![Build](https://github.com/OWNER/REPO/workflows/Build/badge.svg)
![Test](https://github.com/OWNER/REPO/workflows/Test/badge.svg)
![Security](https://github.com/OWNER/REPO/workflows/Security%20Scan/badge.svg)
```

## Secrets Configuration

Required GitHub Secrets:

- `GPG_PRIVATE_KEY` - GPG key for signing releases (optional)
- `GPG_PASSPHRASE` - GPG key passphrase (optional)

## Caching Strategy

### Cross-Compiler Cache

- **Key:** `${{ runner.os }}-cross-compiler-x86_64-elf-gcc-13.2.0`
- **Paths:** `/opt/cross`, `~/.cache/cross-compiler`
- **Benefit:** Saves 10-15 minutes per build

### Docker Cache

- **Type:** GitHub Actions cache
- **Benefit:** Faster Docker builds

### Build Artifacts Cache

- **Strategy:** Upload/download between jobs
- **Benefit:** Avoid rebuilding

## Performance Targets

| Workflow | Target Time | Typical Time |
|----------|-------------|--------------|
| Build    | 15 min      | 12 min       |
| Test     | 15 min      | 12 min       |
| Security | 20 min      | 18 min       |
| Fuzzing  | 60 min      | 60 min       |
| Release  | 30 min      | 25 min       |
| Nightly  | 90 min      | 85 min       |

## Artifact Retention

| Artifact Type        | Retention |
|---------------------|-----------|
| Build artifacts     | 30 days   |
| Test results        | 30 days   |
| Security reports    | 90 days   |
| Fuzzing results     | 90 days   |
| Release artifacts   | 90 days   |
| Nightly builds      | 7 days    |

## Troubleshooting

### Build Failures

1. Check workflow logs in GitHub Actions UI
2. Download failed artifacts
3. Reproduce locally:
   ```bash
   docker-compose run build
   ```

### Test Failures

1. View serial logs in artifacts
2. Run locally:
   ```bash
   make test
   python3 tests/integration/test_boot.py --verbose
   ```

### Security Findings

1. Review security report in artifacts
2. Check GitHub Security tab
3. Run locally:
   ```bash
   make security-check
   ```

### Cache Issues

Clear workflow caches:
```bash
gh cache delete <cache-key>
```

## Local Testing

Test workflows locally with [act](https://github.com/nektos/act):

```bash
# Install act
brew install act  # macOS
# or
curl https://raw.githubusercontent.com/nektos/act/master/install.sh | sudo bash

# Run build workflow
act -j build-linux

# Run test workflow
act -j unit-tests

# List workflows
act -l
```

## Contributing

When adding new workflows:

1. Follow naming convention: `name.yml`
2. Add documentation in this README
3. Test locally with `act` if possible
4. Ensure proper error handling
5. Add appropriate timeout limits
6. Include artifact upload for debugging

## References

- [GitHub Actions Documentation](https://docs.github.com/en/actions)
- [Workflow Syntax](https://docs.github.com/en/actions/reference/workflow-syntax-for-github-actions)
- [Caching Dependencies](https://docs.github.com/en/actions/guides/caching-dependencies-to-speed-up-workflows)
- [AutomationOS CI/CD Documentation](../../docs/CI_CD.md)

---

**Last Updated:** 2026-05-26
