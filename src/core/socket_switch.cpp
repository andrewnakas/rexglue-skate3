/**
 * @file        core/socket_switch.cpp
 * @brief       Socket helpers on Horizon.
 *
 * libnx routes BSD sockets through the bsd:u service and exposes the ordinary
 * close/ioctl spellings, so this is the POSIX file unchanged apart from the
 * platform assertion. Sockets only come up for the guest's networking, which
 * needs socketInitializeDefault to have run - see main_switch.cpp.
 */

#include <rex/net/socket.h>
#include <rex/platform.h>

static_assert(REX_PLATFORM_SWITCH, "This file is Horizon-only");

#include <sys/ioctl.h>
#include <unistd.h>

namespace rex::net {

int socket_close(SocketHandle handle) {
  return close(static_cast<int>(handle));
}

int socket_ioctl(SocketHandle handle, uint32_t cmd, uint8_t* arg) {
  return ioctl(static_cast<int>(handle), cmd, arg);
}

}  // namespace rex::net
