//  usb-host-enumeration-fix.cpp - definition for the LDF discovery marker.
//  This translation unit is the one that pulls the library's usbh.c into the
//  link (the 1024 B control-buffer override, gated on ARDUINO_HOST_ENV).
#include "host-overrides.h"

int usb_host_enumeration_fix_present(void) { return 1; }
