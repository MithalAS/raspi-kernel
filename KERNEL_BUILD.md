# Remora custom kernels

We need some additional changes and driver support, and this is how we should build and maintain them.

## Workflows

### 1. kernel-build.yml (CI - Lightweight Testing)

**Triggers:** Commits and PRs.

Runs on every push and PR to verify builds work:

- Compiles kernel for both arm64 and arm
- Validates XR20M117X driver is properly configured
- **Uploads artifacts** (tarball + checksums + config) for 30 days
- Fails fast if anything is broken

Useful for bisecting issues, testing patches on specific commits.

### 2. kernel-release.yml (Release - Packaged Artifacts)

**Triggers:**

- Manually via `workflow_dispatch` in GitHub UI (Actions tab)
- Automatically on git tags (`v*` or `release-*`)

Runs the same build + validation, then:

- Creates `kernel-{arch}-{short_sha}.tar.gz` (boot/ + lib/)
- Generates `kernel-{arch}-{short_sha}.sha256` checksums
- **Attaches artifacts to GitHub Release** (if on a tag) — **permanent**
- Stores configs in CI artifacts for 30 days (if manual trigger)

## Build Flow

```txt
Commit
    ↓
[kernel-build.yml runs]
  - Compile + validation
  - Uploads artifacts (30 days)
    ↓
Developer pushes tag (v1.0, release-2026-08-10)
    ↓
[kernel-release.yml runs]
  - Same build + validation
  - Packages + attaches to GitHub Release
  - Permanent, citable record
```

## Traceability Strategy

### 1. Commit Hash in Kernel Version String

Every build embeds the short commit SHA in the kernel version. After deployment, you can identify the exact source code with:

```bash
# On a device running the kernel
uname -r
# Output: 5.10.110-v7l+-rem-3f9a21c

# Trace back to source
git show 3f9a21c
```

This works regardless of how the kernel file reaches the device (manual scp, dd, etc.).

### 2. Artifact Tarballs with Checksums

For each build, we create:

- `kernel-{arch}-{short_sha}.tar.gz` — contains boot/, lib/ (zImage, modules, dtbs)
- `kernel-{arch}-{short_sha}.sha256` — SHA256 checksums for verification

Verify downloads:

```bash
sha256sum -c kernel-bcm2711-arm64-3f9a21c.sha256
```

### 3. GitHub Releases

For tagged releases (`v*`, `release-*`), artifacts are automatically attached to a GitHub Release with:

- Permanent, citable URLs
- Action logs linked for full audit trail
- Checksums for reproducibility verification

## Build Configuration

- **defconfig** — Base Raspberry Pi kernel config
- **remora_fragment.config** — Remora-specific overrides (XR20M117X serial driver, etc.)
- **localversion-rem** — Commit hash appended to kernel version string

The workflow uses `CONFIG_LOCALVERSION="-rem"` which the kernel build system automatically appends to the base version string.

### Driver Validation

Each build validates that the **XR20M117X serial driver** is present and correctly configured:

- ✅ **CONFIG_SERIAL_XR20M117X** — Main driver config is enabled
- ✅ **CONFIG_SERIAL_XR20M117X_CORE=y** — Driver is statically linked into kernel
- ✅ **Object file presence** — Verifies `xrm117x.o` exists in build output (confirms compilation)

The validation step fails the entire build if the driver is missing or incorrectly configured, ensuring every release includes the required hardware support.

**Why static linking?** The XR20M117X driver is essential hardware support — building it statically ensures:

- Driver is always present, always loaded (no risk of forgetting to load module)
- Simpler deployment (no separate module management)

## Build Artifacts

### From CI/CD (30-day retention)

Every commit/PR generates:

```txt
kernel-bcm2711-arm64-{short_sha}.tar.gz
kernel-bcm2711-arm64-{short_sha}.sha256
kernel-bcm2711-arm-{short_sha}.tar.gz
kernel-bcm2711-arm-{short_sha}.sha256
.config (final kernel configuration)
```

Access from GitHub Actions → workflow run → Artifacts.

### From GitHub Releases (permanent)

Tagged releases create a GitHub Release with the same artifacts attached — permanent, citable, checksummed record linked to full build logs.

## Reproducing a Build

Given a deployed device reporting `uname -r: 5.10.110-v7l+-rem-3f9a21c`:

1. Identify the commit:

   ```bash
   git show 3f9a21c
   ```

2. Find the release or CI artifacts:
   - Check GitHub Releases for tag matching that commit
   - Check CI Artifacts for the build run
   - Look up the Action run ID in the build logs

3. Download and verify:

   ```bash
   sha256sum -c kernel-bcm2711-arm-3f9a21c.sha256
   tar tzf kernel-bcm2711-arm-3f9a21c.tar.gz
   ```

## Manual Installation

Extract to a temporary directory and copy files to your device:

```bash
tar xzf kernel-bcm2711-arm-3f9a21c.tar.gz -C /tmp/kernel-extract
# Copy boot files to /boot
sudo cp /tmp/kernel-extract/boot/zImage /boot/kernel7l.img
# Install modules
sudo cp -r /tmp/kernel-extract/lib/modules/* /lib/modules/
```
