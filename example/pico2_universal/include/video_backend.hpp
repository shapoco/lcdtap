#pragma once

// Indirection over the two video output backends (HSTX DVI-D and composite
// NTSC/PAL) for the operations that are called from more than one place.
//
// Only the steady-state operations are dispatched here. Clock and peripheral
// init are deliberately left as a plain if/else in main(): those two calls
// take different timing types, happen exactly once during a linear boot
// sequence, and a mistake in them bricks the board — so they keep their real
// signatures rather than being funnelled through void*.

struct VideoBackend {
  void (*launchCore1)(void *s);
  bool (*consumeNewFrame)(void *s);
  void (*flashAcquire)(void *s);
  void (*flashRelease)(void *s);
};
