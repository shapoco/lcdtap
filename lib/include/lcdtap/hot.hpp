#pragma once

// Placement hooks for the per-scanline fill path (LcdTap::fillScanline and
// the OSD overlay), which output drivers call from Core 1 for every active
// line.
//
// On RP2350 the composite output leaves Core 1 almost no headroom per line
// (PAL worst of all), and Core 0 executes from flash; a flash-resident fill
// path makes the fill time depend on XIP cache and bus contention, which
// shows up as slot-fill jitter and, past the budget, as dropped/repeated
// line groups. These macros move the fill path and its lookup data into
// SRAM via the SDK's .time_critical sections (collected into RAM by the
// default linker script). no-tree-loop-distribute-patterns keeps GCC from
// converting the border-clear loops back into calls to flash-resident
// memset, which would defeat the placement.
//
// Code and data need distinct section names: GCC refuses to put read-only
// data into a section it has already marked executable within the same
// translation unit.
//
// On other targets (host tests, ESP32) both macros are no-ops.
#ifdef PICO_RP2350
#define LCDTAP_RAM_FUNC                            \
  __attribute__((section(".time_critical.lcdtap"), \
                 optimize("no-tree-loop-distribute-patterns")))
#define LCDTAP_RAM_DATA __attribute__((section(".time_critical.lcdtap_data")))
#else
#define LCDTAP_RAM_FUNC
#define LCDTAP_RAM_DATA
#endif
