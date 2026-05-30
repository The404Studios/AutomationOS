# GitHub Actions Setup Guide

This guide walks you through setting up GitHub Actions for your AutomationOS fork or deployment.

---

## Prerequisites

- GitHub repository with AutomationOS code
- Admin access to repository
- GitHub account

---

## Step 1: Enable GitHub Actions

1. Go to your repository on GitHub
2. Click **Settings**
3. Navigate to **Actions** → **General**
4. Under **Actions permissions**, select:
   - ✅ **Allow all actions and reusable workflows**
5. Click **Save**

---

## Step 2: Configure Repository Secrets (Optional)

For release signing with GPG:

1. Go to **Settings** → **Secrets and variables** → **Actions**
2. Click **New repository secret**
3. Add the following secrets:

| Secret Name | Description | Required |
|-------------|-------------|----------|
| `GPG_PRIVATE_KEY` | Your GPG private key | Optional |
| `GPG_PASSPHRASE` | GPG key passphrase | Optional |

**To export your GPG key:**
```bash
gpg --export-secret-keys --armor YOUR_KEY_ID > private-key.asc
# Copy the content to GitHub Secret
```

---

## Step 3: Verify Workflows

1. Navigate to the **Actions** tab
2. You should see the following workflows:
   - ✅ Build
   - ✅ Test
   - ✅ Security Scan
   - ✅ Fuzzing
   - ✅ Release
   - ✅ Nightly Build

---

## Step 4: Test the Pipeline

### Test Build Workflow

1. Make a small change (e.g., update README.md)
2. Commit and push:
   ```bash
   git add README.md
   git commit -m "docs: test CI/CD pipeline"
   git push origin main
   ```
3. Go to **Actions** tab
4. Watch the **Build** workflow run
5. Verify all jobs pass (green checkmarks)

### Test Pull Request Workflow

1. Create a new branch:
   ```bash
   git checkout -b test/ci-pipeline
   ```
2. Make a change and push:
   ```bash
   git add <file>
   git commit -m "test: verify CI pipeline"
   git push origin test/ci-pipeline
   ```
3. Create a Pull Request on GitHub
4. Watch CI checks run automatically
5. All checks should pass before merge

---

## Step 5: Configure Branch Protection

Require CI checks before merging:

1. Go to **Settings** → **Branches**
2. Click **Add branch protection rule**
3. For **Branch name pattern**, enter: `main`
4. Enable:
   - ✅ **Require status checks to pass before merging**
   - ✅ **Require branches to be up to date before merging**
5. Select required status checks:
   - ✅ `build-linux`
   - ✅ `build-macos`
   - ✅ `unit-tests`
   - ✅ `integration-tests`
   - ✅ `static-analysis`
6. Enable:
   - ✅ **Require linear history**
   - ✅ **Include administrators**
7. Click **Create**

---

## Step 6: Configure Notifications

Get notified of CI failures:

1. Go to **Settings** → **Notifications**
2. Enable:
   - ✅ **Actions** - Notify me on workflow runs
3. Configure notification preferences:
   - Email
   - GitHub notifications
   - Slack (via webhooks)

---

## Step 7: Add Status Badges to README

Add to your README.md:

```markdown
## Build Status

[![Build](https://github.com/USERNAME/AutomationOS/workflows/Build/badge.svg)](https://github.com/USERNAME/AutomationOS/actions?query=workflow%3ABuild)
[![Test](https://github.com/USERNAME/AutomationOS/workflows/Test/badge.svg)](https://github.com/USERNAME/AutomationOS/actions?query=workflow%3ATest)
[![Security Scan](https://github.com/USERNAME/AutomationOS/workflows/Security%20Scan/badge.svg)](https://github.com/USERNAME/AutomationOS/actions?query=workflow%3A%22Security+Scan%22)
```

Replace `USERNAME` with your GitHub username.

---

## Step 8: Configure Docker Registry (Optional)

For Docker image publishing:

1. Go to **Settings** → **Secrets and variables** → **Actions**
2. GitHub Container Registry (ghcr.io) uses `GITHUB_TOKEN` automatically
3. No additional configuration needed!

Images will be published to:
```
ghcr.io/USERNAME/automationos/build:latest
ghcr.io/USERNAME/automationos/build:v1.0.0
```

---

## Step 9: Test Release Workflow

Create a test release:

1. Update version and changelog
2. Create and push a tag:
   ```bash
   git tag -a v0.1.0-alpha -m "Alpha release"
   git push origin v0.1.0-alpha
   ```
3. Go to **Actions** tab
4. Watch **Release** workflow run
5. Check **Releases** page for the new release

---

## Step 10: Configure Nightly Builds

Nightly builds are configured to run at 2 AM UTC.

**To change the schedule:**

1. Edit `.github/workflows/nightly.yml`
2. Modify the cron expression:
   ```yaml
   schedule:
     - cron: '0 2 * * *'  # 2 AM UTC daily
   ```
3. Commit and push

**Cron format:**
```
┌───────────── minute (0 - 59)
│ ┌───────────── hour (0 - 23)
│ │ ┌───────────── day of month (1 - 31)
│ │ │ ┌───────────── month (1 - 12)
│ │ │ │ ┌───────────── day of week (0 - 6) (Sunday to Saturday)
│ │ │ │ │
* * * * *
```

**Examples:**
- `0 2 * * *` - Daily at 2 AM UTC
- `0 2 * * 1` - Every Monday at 2 AM UTC
- `0 */6 * * *` - Every 6 hours

---

## Troubleshooting

### Workflow Not Running

**Problem:** Workflow doesn't trigger on push

**Solutions:**
1. Check Actions are enabled (Settings → Actions)
2. Verify branch name matches trigger (e.g., `main` vs `master`)
3. Check workflow syntax:
   ```bash
   # Install actionlint
   brew install actionlint  # macOS
   # or
   go install github.com/rhysd/actionlint/cmd/actionlint@latest

   # Check syntax
   actionlint .github/workflows/*.yml
   ```

### Build Failures

**Problem:** Build fails with "cross-compiler not found"

**Solutions:**
1. Check cache is working
2. Verify toolchain installation script
3. Use Docker build as fallback

### Permission Errors

**Problem:** "Resource not accessible by integration"

**Solutions:**
1. Check workflow permissions in Settings → Actions
2. Ensure `GITHUB_TOKEN` has required permissions:
   ```yaml
   permissions:
     contents: write
     packages: write
   ```

### Timeout Errors

**Problem:** Workflow times out

**Solutions:**
1. Increase timeout in workflow:
   ```yaml
   jobs:
     build:
       timeout-minutes: 30
   ```
2. Optimize build (caching, parallel jobs)

### Cache Issues

**Problem:** Cache not working or stale

**Solutions:**
1. Clear cache:
   ```bash
   gh cache delete <cache-key>
   ```
2. Update cache key in workflow
3. Check cache size limits (10 GB per repo)

---

## Advanced Configuration

### Parallel Jobs

Reduce build time by running jobs in parallel:

```yaml
jobs:
  build:
    strategy:
      matrix:
        os: [ubuntu-22.04, macos-latest]
        config: [debug, release]
    runs-on: ${{ matrix.os }}
```

### Self-Hosted Runners

For faster builds or specific hardware:

1. Go to **Settings** → **Actions** → **Runners**
2. Click **New self-hosted runner**
3. Follow setup instructions
4. Update workflows:
   ```yaml
   runs-on: self-hosted
   ```

### Reusable Workflows

Create reusable workflow components:

```yaml
# .github/workflows/build-common.yml
on:
  workflow_call:
    inputs:
      platform:
        required: true
        type: string

jobs:
  build:
    runs-on: ${{ inputs.platform }}
    # ... steps
```

Use in other workflows:
```yaml
jobs:
  call-build:
    uses: ./.github/workflows/build-common.yml
    with:
      platform: ubuntu-22.04
```

---

## Monitoring and Metrics

### View Workflow Runs

```bash
# Install GitHub CLI
brew install gh  # macOS
# or
sudo apt install gh  # Ubuntu

# List workflow runs
gh run list

# View specific run
gh run view <run-id>

# Watch live
gh run watch
```

### Download Artifacts

```bash
# List artifacts
gh run list

# Download artifacts
gh run download <run-id>

# Download specific artifact
gh run download <run-id> -n artifact-name
```

### Check Workflow Status

```bash
# View workflow status
gh workflow view

# List workflows
gh workflow list

# Run workflow manually
gh workflow run build.yml
```

---

## Cost Optimization

GitHub Actions usage:

**Free Tier:**
- Public repos: Unlimited minutes
- Private repos: 2,000 minutes/month

**Tips to reduce usage:**
1. ✅ Use caching extensively
2. ✅ Run expensive jobs (fuzzing) only on main branch
3. ✅ Use `if` conditions to skip unnecessary jobs
4. ✅ Optimize Docker images (layer caching)
5. ✅ Use self-hosted runners for heavy workloads

---

## Security Best Practices

1. **Never commit secrets** to repository
2. **Use GitHub Secrets** for sensitive data
3. **Limit workflow permissions** to minimum required
4. **Review third-party actions** before use
5. **Enable Dependabot** for automatic updates
6. **Use signed commits** for releases
7. **Scan dependencies** regularly

---

## Next Steps

1. ✅ Enable all workflows
2. ✅ Configure branch protection
3. ✅ Add status badges to README
4. ✅ Set up notifications
5. ✅ Create first release
6. ✅ Monitor workflow runs
7. ✅ Optimize as needed

---

## Resources

- [GitHub Actions Documentation](https://docs.github.com/en/actions)
- [Workflow Syntax Reference](https://docs.github.com/en/actions/reference/workflow-syntax-for-github-actions)
- [GitHub CLI Documentation](https://cli.github.com/manual/)
- [AutomationOS CI/CD Docs](CI_CD.md)

---

## Support

If you encounter issues:

1. Check [Troubleshooting](#troubleshooting) section
2. Review workflow logs in GitHub Actions
3. Consult [CI/CD documentation](CI_CD.md)
4. Open an issue with logs and details

---

**Last Updated:** 2026-05-26
