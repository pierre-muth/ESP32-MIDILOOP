//  host-overrides.h - LDF discovery marker for lib/usb-host-enumeration-fix.
//
//  Include this header from any source file of a project that runs the
//  ESP32-S3 USB in host mode so PlatformIO's Library Dependency Finder
//  picks up the library and links its usbh.c (CFG_TUH_ENUMERATION_BUFSIZE
//  is 1024 there, vs 256 in the core's prebuilt archive member) ahead of
//  libarduino_tinyusb.a. See the library's description for the background,
//  and platformio.ini (ARDUINO_HOST_ENV) for the mode switch.
#pragma once

#ifdef ARDUINO_HOST_ENV
static_assert(ARDUINO_HOST_ENV, "host enumeration override active");
#endif
extern "C" int usb_host_enumeration_fix_present(void);
