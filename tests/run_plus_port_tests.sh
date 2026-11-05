#!/usr/bin/env bash
set -euo pipefail
TASK_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$TASK_ROOT"
mkdir -p .local
python3 tests/test_plus_font_resources.py
python3 scripts/check_plus_font_resources.py
python3 tests/test_plus_book_titles.py
clang++ -std=c++20 -Wall -Wextra -Werror tests/test_plus_framebuffer.cpp -o .local/test_plus_framebuffer
.local/test_plus_framebuffer
python3 tests/test_battery_stability.py
python3 tests/test_plus_battery_curve.py
python3 tests/test_plus_battery_awake_sampling.py
python3 tests/test_plus_idle_power.py
clang -Ifirmware/rickyos-plus-1.2/upstream/freeink-sdk/libs/font/FreeInkFont/include -c firmware/rickyos-plus-1.2/upstream/freeink-sdk/libs/font/FreeInkFont/src/FontAlloc.c -o .local/plus-font-alloc.o
clang -c firmware/rickyos-plus-1.2/upstream/lib/uzlib/src/tinflate.c -o .local/plus-font-inflate.o
clang++ -std=c++20 -Wall -Wextra -Werror -Wno-missing-field-initializers -Wl,-dead_strip \
  -Itests/font-host-stubs \
  -Ifirmware/rickyos-plus-1.2/upstream/lib/InflateReader \
  -Ifirmware/rickyos-plus-1.2/upstream/lib/Utf8 \
  -Ifirmware/rickyos-plus-1.2/upstream/freeink-sdk/libs/font/FreeInkFont/include \
  -Ifirmware/rickyos-plus-1.2/upstream/lib/EpdFont \
  -Ifirmware/rickyos-plus-1.2/generated \
  -Ifirmware/rickyos-plus-1.2/upstream/lib/uzlib/src \
  tests/test_plus_fonts.cpp \
  firmware/rickyos-plus-1.2/upstream/lib/EpdFont/FontDecompressor.cpp \
  firmware/rickyos-plus-1.2/upstream/lib/InflateReader/InflateReader.cpp \
  firmware/rickyos-plus-1.2/upstream/lib/Utf8/Utf8.cpp \
  .local/plus-font-alloc.o .local/plus-font-inflate.o -o .local/test_plus_fonts
.local/test_plus_fonts

python3 tests/test_plus_clock_owner.py
python3 tests/test_plus_clock_radio.py
python3 tests/test_plus_net_alloc_trace.py
python3 tests/test_plus_memory_logging.py
python3 tests/test_plus_field_diagnostics.py
python3 tests/test_plus_font_network_auto.py
python3 tests/test_plus_wifi_security.py
python3 tests/test_plus_radio_auto.py
python3 tests/test_plus_private_network_profile.py
python3 tests/test_plus_wpa3_report.py

clang++ -std=c++17 -Wall -Wextra -Werror tests/test_plus_http_date.cpp -o .local/test_plus_http_date
.local/test_plus_http_date

# Plus peripherals, shared input, display and recorder regression suites.
python3 tests/test_plus_audio.py
python3 tests/test_plus_frontlight_panel.py
python3 tests/test_plus_standby_frontlight.py
python3 tests/test_plus_standby_sleep.py
python3 tests/test_plus_static_sleep.py
python3 tests/test_plus_p14_restore_audit.py
python3 tests/test_plus_standby_home.py
python3 tests/test_plus_standby_swipe_unlock.py
python3 tests/test_plus_button_mapping.py
python3 tests/test_plus_landscape_keys.py
python3 tests/test_plus_touch_tolerant_idle.py
python3 tests/test_plus_button_settings.py
python3 tests/test_plus_control_center_layout.py
python3 tests/test_plus_haptic.py
python3 tests/test_plus_haptic_input.py
python3 tests/test_plus_recorder.py
python3 tests/test_plus_recorder_ui.py
python3 tests/test_plus_regional_gray.py
python3 tests/test_plus_battery_alignment.py
python3 tests/test_plus_ui_contrast.py
python3 tests/test_plus_ui_text_roles.py
python3 tests/test_plus_ui_black_margin.py
python3 tests/test_plus_ui_white_maintenance.py
python3 tests/test_plus_quiet_settings.py
python3 tests/test_plus_holding_hand.py
python3 tests/test_native_page_turn.py
python3 tests/test_main_tab_motion.py
python3 tests/test_plus_main_tab_pipeline.py
python3 tests/test_native_gray_refresh_policy.py
python3 tests/test_plus_reader_chrome.py
python3 tests/test_plus_compact_reader_chrome.py
python3 tests/test_plus_compact_reader_margins.py
python3 tests/test_plus_reader_refresh_geometry.py
python3 tests/test_plus_reader_refresh_cadence.py
python3 tests/test_plus_reader_exit.py
python3 tests/test_plus_reading_background.py
python3 tests/test_plus_image_page_polarity.py
python3 tests/test_plus_regional_raster.py
python3 tests/test_plus_transparent_wallpaper.py
python3 tests/test_plus_usb.py
python3 tests/test_plus_usb_remount.py
python3 tests/test_plus_download_cancel.py

python3 tests/test_plus_lcd_clock_lifecycle.py
python3 tests/test_plus_psram_resume.py
python3 tests/test_plus_lcd_restore_failures.py
python3 tests/test_plus_lcd_first_frame.py
python3 tests/test_plus_lcd_tail.py
python3 tests/test_plus_line_timing.py
python3 tests/test_plus_phase_lookahead.py
python3 tests/test_plus_timed_neutral.py

python3 tests/test_plus_font_reconnect_progress.py
python3 tests/test_plus_index_budget.py
python3 tests/test_plus_static_standby.py

python3 tests/test_plus_tls_retry.py
python3 tests/test_plus_font_source.py
python3 tests/test_plus_web_responsiveness.py
python3 tests/test_plus_web_memory.py
python3 ../../scripts/tests/test_product_userguide.py
python3 tests/test_plus_boot_identity.py
python3 tests/test_plus_reader_page_cache.py
python3 tests/test_plus_reader_paint_speed.py
python3 tests/test_plus_ttf_block_cache.py
python3 scripts/generate_plus_common_chars.py --check
python3 tests/test_plus_reader_image_cancel.py
python3 tests/test_plus_image_quality.py
python3 tests/test_plus_reader_image_layout.py
python3 tests/test_plus_reader_image_cache.py
python3 tests/test_plus_reader_native_images.py
python3 tests/test_plus_reader_waveform.py
python3 tests/test_plus_reader_ink.py
python3 tests/test_plus_reader_retained_ink.py
python3 tests/test_plus_reader_quality.py
python3 tests/test_plus_reader_early_erase.py
python3 tests/test_plus_reader_erase_overlap.py
python3 tests/test_plus_reader_edge_erase.py
python3 tests/test_plus_reader_white_preimage.py
python3 tests/test_plus_reader_fine_ripple.py
python3 tests/test_plus_turn_selftest.py
python3 tests/test_plus_native_cleanup_override.py
python3 tests/test_plus_reader_motion_cadence.py
python3 tests/test_plus_direct_calibration.py
python3 tests/test_plus_usb_retry.py
python3 tests/test_plus_ota_manifest.py
python3 tests/test_plus_kindle_document.py
python3 tests/test_plus_kindle_import.py
python3 tests/test_plus_pico_features.py
python3 tests/test_plus_source_recording.py
python3 tests/test_plus_storage_summary.py
python3 tests/test_plus_release_idf.py
python3 ../../scripts/tests/test_skill_gomoku.py
