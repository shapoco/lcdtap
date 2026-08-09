#pragma once

// JSON settings commands (hello / getrigconfig / setrigconfig / netstatus)
// on the rig's PC-facing CDC console, spoken by the docs/test-pico2/ Web
// Serial settings page.

namespace testrig {

// Pump from the core 0 main loop: reads console input line by line and
// answers JSON commands. Non-JSON input is ignored.
void configCmdsProcess();

}  // namespace testrig
