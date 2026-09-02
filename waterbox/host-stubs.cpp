// What the emulator library expects its user interface to provide, answered
// headlessly: the "wait while pumping events" helper spins the machine's
// scheduler instead, a fatal error is a message and an abort.
// SPDX-License-Identifier: MIT
#include "stdafx.h"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string_view>

#include "vsched.h"

void qt_events_aware_op(int /*repeat_duration_ms*/, std::function<bool()> wrapped_op)
{
  // upstream pumps the Qt event loop between attempts; here the other
  // threads of the machine are what needs running
  while (!wrapped_op())
  {
    vsched_yield_default();
  }
}

[[noreturn]] void report_fatal_error(std::string_view text, bool /*is_html*/, bool /*include_help_text*/)
{
  std::fprintf(stderr, "rpcs3 fatal: %.*s\n", static_cast<int>(text.size()), text.data());
  std::fflush(stderr);
  std::abort();
}

// The input configuration set lives in a Qt dialog upstream; the pad thread
// only wants it to exist.
#include "Emu/Io/pad_config.h"
cfg_input_configurations g_cfg_input_configs;

// Mouse-driven gyro belongs to a window; a core has no mouse.
#include "Input/mouse_gyro_handler.h"
void mouse_gyro_handler::set_enabled(bool) {}
void mouse_gyro_handler::apply_gyro(const std::shared_ptr<Pad>&) {}

// The command-line input-config override lives in rpcs3's main(); a core
// takes its input configuration from the package.
std::string g_input_config_override;
