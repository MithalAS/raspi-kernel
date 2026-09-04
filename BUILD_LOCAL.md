# Local Kernel Build

Cross-compile the Remora kernel on your x86 machine for faster iteration.

## Setup (One-time)

Install the ARM32 cross-compiler and build dependencies:

```bash
sudo apt-get update
sudo apt-get install -y \
  gcc-arm-linux-gnueabihf \
  build-essential \
  flex \
  bison \
  libssl-dev
```

Verify installation:

```bash
arm-linux-gnueabihf-gcc --version
openssl version
```

## Building

From the repository root:

```bash
./build-local.sh
```

This will:

1. Configure kernel with `bcm2711_defconfig` + Remora overrides (via `merge_config.sh`)
2. Verify every symbol in `remora_fragment.config` survived config resolution
3. Cross-compile for ARM32 (armhf)
4. Validate XR20M117X driver and overlayfs support are present
5. Install modules + boot files, package into tarball + checksums
6. Clean, colorized output

## Remora customizations

All Remora-specific kernel options live in `remora_fragment.config`. This is
merged onto `bcm2711_defconfig` with `scripts/kconfig/merge_config.sh`, which
prints every value it overrides, then resolved with `make olddefconfig`.

After resolution the script verifies that **every** `CONFIG_X=value` line in the
fragment appears verbatim in the final `.config`, and fails the build otherwise.
This catches options silently dropped due to missing dependencies.

### Docker / overlay2 support

Docker's `overlay2` storage driver needs overlayfs. `bcm2711_defconfig` ships it
as a *module* (`CONFIG_OVERLAY_FS=m`) with its sub-options disabled, so the
fragment overrides it:

```
CONFIG_OVERLAY_FS=y
CONFIG_OVERLAY_FS_REDIRECT_DIR=y
CONFIG_OVERLAY_FS_METACOPY=y
```

It is built in (`=y`) rather than a module so `overlay` is always present in
`/proc/filesystems` at boot, with no dependency on `depmod`/module loading on
the target. `CONFIG_OVERLAY_FS_REDIRECT_ALWAYS_FOLLOW=y` is pulled in
automatically.

The build fails if these are not enabled. To confirm on the target:

```bash
grep overlay /proc/filesystems       # expect: nodev overlay
docker info | grep "Storage Driver"  # expect: overlay2
```

Everything else Docker requires (cgroups, namespaces, veth, bridge,
netfilter/NAT, seccomp, memcg, ext4 xattrs/ACLs) is already enabled by
`bcm2711_defconfig`.

### Clean build (from scratch)

```bash
./build-local.sh clean
```

Removes `build/` and `install/` directories, forces full rebuild.

## Output

After a successful build, you'll find:

```
kernel-bcm2711-arm-{short_sha}.tar.gz   # Boot + modules
kernel-bcm2711-arm-{short_sha}.sha256   # Checksums
build/.config                            # Final kernel config
install/boot/kernel7l.img                # Kernel image
install/boot/*.dtb, install/boot/overlays/  # Device tree blobs
install/lib/modules/{version}/           # Installed modules
```

## Troubleshooting Build Errors

If the build fails, the script stops with a clear error message. Common issues:

### Warnings-as-errors (`-Werror`)

This tree is Linux 5.10, which has **no `CONFIG_WERROR` symbol** (it was added
in 5.15). Setting it in `.config` has no effect — `olddefconfig` silently drops
unknown symbols. Builds are therefore *not* warnings-as-errors by default.

To opt in locally:

```bash
WERROR=1 ./build-local.sh
```

This passes `KCFLAGS=-Werror` to the build instead.

### Fragment option not applied

If the build fails with `fragment requested 'CONFIG_X=y' but config has ...`,
the option was dropped during `olddefconfig`, almost always because a
dependency is unmet. Check the option's `Kconfig` entry for its `depends on`
line and add the missing prerequisites to `remora_fragment.config`.

### `array_index_nospec` or other ATA errors

These often indicate a kernel version or defconfig mismatch. Check:

1. Which kernel version are you building? (`cat Makefile | head -5`)
2. Is `bcm2711_defconfig` correct for this version?
3. Try disabling specific drivers if needed

### Driver not found

If `CONFIG_SERIAL_XR20M117X_CORE` validation fails:

1. Check `build/.config` has the driver enabled: `grep SERIAL_XR20M117X build/.config`
2. Verify `remora_fragment.config` exists and is correct
3. Re-run: `./build-local.sh clean`

### Missing OpenSSL headers (`openssl/bio.h`)

If you get: `fatal error: openssl/bio.h: No such file or directory`

Install OpenSSL dev:

```bash
sudo apt-get install -y libssl-dev
```

### Missing tools (`flex`, `bison`)

If you get errors about missing `flex` or `bison`:

```bash
sudo apt-get install -y flex bison
```

## Comparing Local vs CI

Local build uses the same:

- Defconfig (bcm2711_defconfig) + `remora_fragment.config`
- Validation steps
- Artifact format and install layout

Differences:

- CI also builds arm64 in parallel.
- CI applies the fragment with `cat >> .config` + `oldconfig`; the local script
  uses `merge_config.sh` + `olddefconfig` and additionally verifies the fragment
  was fully applied. Consider aligning `.github/workflows/kernel-build.yml` so
  CI catches dropped options too.
- CI sets `CONFIG_WERROR=y`, which is a no-op on this 5.10 tree (see above).

## Speed

On a modern CPU, expect:

- First build (full): ~8-15 minutes
- Subsequent builds (incremental): ~2-5 minutes
- Rebuilding after config change: ~5-10 minutes

Using `-j $(nproc)` parallelizes across all CPU cores.
