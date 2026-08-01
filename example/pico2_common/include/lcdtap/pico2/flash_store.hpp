#pragma once

// CRC-32-protected configuration blob storage in the last flash sectors.
//
// Each blob occupies one sector, addressed as "N sectors before the end of
// flash" so different stores (device config, network config) never collide.
// A size change invalidates the CRC, so blobs written by an older layout are
// discarded rather than reinterpreted.

#include <cstddef>
#include <cstdint>

namespace lcdtap::pico2 {

// Read the blob back; returns false when the CRC does not match (blob absent
// or written with a different layout). len + 4 must fit one flash page.
bool flashStoreLoad(uint32_t sectorsFromEnd, void* out, size_t len);

// Erase the sector and program the blob + CRC. Interrupts are disabled for
// the duration; any other core must be parked in SRAM by the caller (the
// erase/program window makes flash-resident code inaccessible).
void flashStoreSave(uint32_t sectorsFromEnd, const void* data, size_t len);

}  // namespace lcdtap::pico2
