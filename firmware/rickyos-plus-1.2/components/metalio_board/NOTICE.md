# Vendor provenance

Copied from Metalio-E-INK4-Plus-source-2.0.76-debug-20261007/firmware,
boards/metalio-e-ink4-plus, boards/common/storage and display/epd_font24.
The vendor root MIT license is preserved in LICENSE.vendor; embedded source
copyright/license comments remain intact.

Changes for the bringup build:

- TCA Port0 safe output values are loaded before switching direction to output.
- TPS startup failures request normal poweroff instead of keeping rails on for probing.
- Deinitialization writes disabled TPS outputs and retains their output direction.
- The EPD facade requests poweroff on startup failure as well.
- No app UI, cloud service, voice stack or LVGL is compiled.

These changes have not been verified on hardware and do not fix ROM download
mode hardware defaults. Board identity and panel VCOM must be checked first.

Additional bringup changes:
- SD mount no longer calls SdEnsureAppLayout; sd_paths.h contains only mount point.
- SD/GPIO C++ configuration structs use zero initialization before assignments.
- portrait_i1.{c,h} is new code for a byte-stride-aware monochrome display bridge.
