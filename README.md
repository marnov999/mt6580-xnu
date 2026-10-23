# mt6580-xnu

Build tree for porting XNU (xnu-4570.71.2, armv7) to the MediaTek MT6580.

- Target userland: iOS 10.3.4 (14G61), iPhone 5,1
- Kernel: xnu-4570.71.2, `-arch armv7`
- Boot: `fastboot boot`; no bootloader/partition flashing
- CI: GitHub Actions macOS runners (`gh` authenticated as marnov999)

Current state: toolchain probe only.
