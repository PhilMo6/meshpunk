#pragma once

// USB flash guards: no-ops in the module — internal-flash write pausing is a
// host-side stdio concern (campaign ledger tracks folding it into the proto_*
// wrappers). Same declaration homes as the firmware header.

struct UsbFlashGuard   { UsbFlashGuard() {} };
struct UsbFlashGuardIf { explicit UsbFlashGuardIf(bool) {} };
inline void usb_flash_guard_begin() {}
inline void usb_flash_guard_end()   {}
