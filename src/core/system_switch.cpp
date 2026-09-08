/**
 * @file        core/system_switch.cpp
 * @brief       Horizon implementation of the rex/system.h helpers.
 *
 * @license     BSD 3-Clause License
 */

#include <cstdio>
#include <string>

#include <switch.h>

#include <rex/platform.h>
#include <rex/system.h>

static_assert(REX_PLATFORM_SWITCH, "This file is Horizon-only");

namespace rex {

void ShowSimpleMessageBox(SimpleMessageBoxType type, std::string_view message) {
  const char* level = "INFO";
  switch (type) {
    case SimpleMessageBoxType::Help:
      level = "INFO";
      break;
    case SimpleMessageBoxType::Warning:
      level = "WARNING";
      break;
    case SimpleMessageBoxType::Error:
      level = "ERROR";
      break;
  }
  // Always to stderr first. It reaches nxlink and the log file, and it is the
  // only copy that survives if the applet below cannot be shown - which is
  // exactly the case when the message is "this build needs Application mode",
  // the one message a player most needs to read.
  std::fprintf(stderr, "[%s] %.*s\n", level, static_cast<int>(message.size()), message.data());
  std::fflush(stderr);

  // The error applet is a library applet: it can only be launched from an
  // application, and it takes over the screen until the player dismisses it,
  // which gives the blocking behaviour this function promises.
  if (appletGetAppletType() != AppletType_Application &&
      appletGetAppletType() != AppletType_SystemApplication) {
    return;
  }

  const std::string text(message);
  // The dialog line is the short one on the error screen; the second is shown
  // when the player asks for details. Sending the same text to both means a
  // long message is still readable somewhere.
  ErrorApplicationConfig config;
  if (R_FAILED(errorApplicationCreate(&config, text.c_str(), text.c_str()))) {
    return;
  }
  errorApplicationShow(&config);
}

}  // namespace rex
