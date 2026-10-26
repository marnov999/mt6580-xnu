# mt6580-xnu

Build tree for porting XNU (xnu-4570.71.2, armv7) to the MediaTek MT6580.

- Target userland: iOS 10.3.4 (14G61), iPhone 5,1
- Kernel: xnu-4570.71.2, `-arch armv7`
- Boot: `fastboot boot`; no bootloader/partition flashing
- CI: GitHub Actions macOS runners (`gh` authenticated as marnov999)

## Current state (2026-10-26)

The ARM32 build-system support and MT6580 machine config are in place.
CI now compiles most of `bsd`, `osfmk` and `iokit` for armv7.  It is not yet
linking.  The VFP2 `vmrs` issue and the missing `os/firehose_buffer_private.h`
are fixed; the current blocker is in `bsd/net/dlil.c`:

- The static assertions require 64-bit fields in `struct if_data_internal`
  to be 8-byte aligned, but on the armv7 build they land at 4-byte alignment.
  Next: confirm whether `#pragma pack(4)` is leaking or whether explicit
  padding / `aligned(8)` is needed.

See the project-level `XNU-ARM-BUILD-STATUS.md` for the full log, exact CI
runs, and next steps.

## Build

```sh
make -C xnu \
  TARGET_CONFIGS="RELEASE ARM MT6580" \
  ARCH_CONFIGS=ARM MACHINE_CONFIGS=MT6580 KERNEL_CONFIGS=RELEASE \
  PLATFORM=iPhoneOS SDKROOT=iphoneos SDKVERSION=10.0 \
  BUILD_LTO=0 BUILD_DSYM=0 BUILD_WERROR=0 \
  CTFCONVERT=/usr/bin/true CTFMERGE=/usr/bin/true CTFINSERT=/usr/bin/true \
  EMBEDDED_DEVICE_MAP=/dev/null \
  OBJROOT="$PWD/build/obj" DSTROOT="$PWD/build/dst" SYMROOT="$PWD/build/sym" \
  all
```

## Key changes

- `makedefs/MakeInc.def`: ARM arch/machine flags, linker flags, dsymutil.
- `makedefs/MakeInc.top`: default ARM machine config `MT6580`.
- `config/MASTER.arm`: `options CONFIG_EMBEDDED`.
- `pexpert/pexpert/arm/board_config.h`: `ARM_BOARD_CONFIG_MT6580`.
- `pexpert/pexpert/arm/MT6580.h`: MT6580 register/timer constants.
- `bsd/kern/makekdebugevents.py`: Python 3 compatibility.
- `.github/workflows/build-arm.yml`: armv7 CI build.
