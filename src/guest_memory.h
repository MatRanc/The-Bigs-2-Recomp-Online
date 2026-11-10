// thebigs2 - guest address -> host pointer for hand-written hooks.
//
// The generated code reaches guest memory through REX_RAW_ADDR, which adds 0x1000 for addresses in the 0xE0000000
// physical view when the host's allocation granularity is coarser than 4 KB (Windows: 64 KB, Apple Silicon: 16 KB
// pages). The SDK maps that view without its 4 KB offset there and compensates on every access. Hooks must do the
// same, or anything in 0xE0000000+ (file buffers, e.g. the unpacked main menu) is read 4 KB off. Linux and Intel Macs
// (4 KB pages) need no offset.
#pragma once

#include <cstdint>

#include <rex/platform.h>

inline uint8_t* GuestPtr(uint8_t* base, uint32_t addr) {
#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && REX_ARCH_ARM64)
  return base + addr + (addr >= 0xE0000000u ? 0x1000u : 0u);
#else
  return base + addr;
#endif
}
