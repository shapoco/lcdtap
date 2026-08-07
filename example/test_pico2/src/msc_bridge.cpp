#include "testrig/msc_bridge.hpp"

#if TESTRIG_MSC_PASSTHROUGH

#include <cstring>

#include "tusb.h"

//--------------------------------------------------------------------+
// Mass-storage passthrough (ported from mimicusb msd.cpp).
//
// The PC-facing MSC *device* callbacks run on core 0; the target-facing MSC
// *host* stack runs on core 1. A single-slot handshake shuttles one block
// transfer at a time across the cores (USB Bulk-Only Transport issues one
// SCSI command at a time, so a single slot is sufficient).
//
// Media insert/remove is emulated with a SCSI Unit Attention (ASC 0x28,
// "MEDIUM MAY HAVE CHANGED") so the PC re-reads capacity and remounts
// whenever the target enters or leaves BOOTSEL.
//--------------------------------------------------------------------+

namespace testrig {

namespace {

// ---- Target state (written on core1, read on core0) ----
volatile uint8_t gTargetAddr = 0;    // USB address of target MSC, 0 = none
volatile bool gTargetReady = false;  // capacity known and usable
volatile uint32_t gBlkCount = 0;
volatile uint16_t gBlkSize = 512;

// Set on every media transition; test_unit_ready reports Unit Attention once.
volatile bool gMediaChanged = false;

// ---- Single-slot cross-core block I/O handshake ----
enum IoState : int {
  IO_IDLE = 0,  // no request
  IO_REQ,       // core0 posted a request
  IO_INFLIGHT,  // core1 issued the async USB transfer
  IO_DONE,      // transfer succeeded, data ready
  IO_ERR,       // transfer failed
};

int gIoState = IO_IDLE;
bool gIoIsWrite = false;
uint32_t gIoLba = 0;
uint16_t gIoCount = 0;  // number of blocks (always 1 here)
uint8_t gIoBuf[512];    // one block

inline int ioLoad() { return __atomic_load_n(&gIoState, __ATOMIC_ACQUIRE); }
inline void ioStore(int s) { __atomic_store_n(&gIoState, s, __ATOMIC_RELEASE); }

// ---- core1: transfer completion ----
bool ioCompleteCb(uint8_t devAddr, tuh_msc_complete_data_t const* cbData) {
  (void)devAddr;
  bool ok = (cbData->csw->status == MSC_CSW_STATUS_PASSED);
  ioStore(ok ? IO_DONE : IO_ERR);
  return true;
}

}  // namespace

void mscBridgeOnMount(uint8_t devAddr) {
  // Capacity was read by the host stack during enumeration.
  gBlkCount = tuh_msc_get_block_count(devAddr, 0);
  gBlkSize = (uint16_t)tuh_msc_get_block_size(devAddr, 0);
  gTargetAddr = devAddr;
  gTargetReady = (gBlkCount > 0 && gBlkSize <= sizeof(gIoBuf));
  ioStore(IO_IDLE);      // clear any stale slot from a previous target
  gMediaChanged = true;  // signal "medium inserted" to the PC
}

void mscBridgeOnUnmount(uint8_t devAddr) {
  if (devAddr == gTargetAddr) {
    gTargetAddr = 0;
    gTargetReady = false;
    gBlkCount = 0;
    gMediaChanged = true;  // signal "medium removed" to the PC
    // Fail any in-flight request so the PC gets an error instead of a hang.
    if (ioLoad() == IO_REQ || ioLoad() == IO_INFLIGHT) {
      ioStore(IO_ERR);
    }
  }
}

// core1: issue the pending transfer.
void mscBridgeHostTask() {
  if (ioLoad() != IO_REQ) return;

  uint8_t addr = gTargetAddr;
  if (addr == 0 || !gTargetReady) {
    ioStore(IO_ERR);
    return;
  }

  bool ok;
  if (gIoIsWrite) {
    ok = tuh_msc_write10(addr, 0, gIoBuf, gIoLba, gIoCount, ioCompleteCb, 0);
  } else {
    ok = tuh_msc_read10(addr, 0, gIoBuf, gIoLba, gIoCount, ioCompleteCb, 0);
  }
  ioStore(ok ? IO_INFLIGHT : IO_ERR);
}

}  // namespace testrig

//--------------------------------------------------------------------+
// TinyUSB device MSC callbacks (PC-facing, core0)
//--------------------------------------------------------------------+

using namespace testrig;

extern "C" {

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8],
                        uint8_t product_id[16], uint8_t product_rev[4]) {
  (void)lun;
  const char vid[] = "shapoco";
  const char pid[] = "TestRig Storage";
  const char rev[] = "1.0";
  memcpy(vendor_id, vid, strlen(vid));
  memcpy(product_id, pid, strlen(pid));
  memcpy(product_rev, rev, strlen(rev));
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
  // Report the media change exactly once so the host re-reads capacity.
  if (gMediaChanged) {
    gMediaChanged = false;
    // medium may have changed
    tud_msc_set_sense(lun, SCSI_SENSE_UNIT_ATTENTION, 0x28, 0x00);
    return false;
  }
  if (!gTargetReady) {
    // medium not present
    tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
    return false;
  }
  return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t* block_count,
                         uint16_t* block_size) {
  (void)lun;
  if (gTargetReady) {
    *block_count = gBlkCount;
    *block_size = gBlkSize;
  } else {
    *block_count = 0;
    *block_size = 512;
  }
}

// Read one block per invocation; TinyUSB re-invokes with an advanced offset
// for multi-block transfers. Returns TUD_MSC_RET_BUSY (0) while core1
// fetches the block from the target, then the byte count once ready.
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          void* buffer, uint32_t bufsize) {
  if (!gTargetReady) {
    tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
    return TUD_MSC_RET_ERROR;
  }

  int state = ioLoad();
  switch (state) {
    case IO_IDLE: {
      gIoIsWrite = false;
      gIoLba = lba + offset / gBlkSize;
      gIoCount = 1;
      ioStore(IO_REQ);
      return TUD_MSC_RET_BUSY;
    }
    case IO_DONE: {
      uint32_t n = gBlkSize < bufsize ? gBlkSize : bufsize;
      memcpy(buffer, gIoBuf, n);
      ioStore(IO_IDLE);
      return (int32_t)n;
    }
    case IO_ERR: {
      ioStore(IO_IDLE);
      // unrecovered read error
      tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x11, 0x00);
      return TUD_MSC_RET_ERROR;
    }
    default:  // IO_REQ / IO_INFLIGHT
      return TUD_MSC_RET_BUSY;
  }
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                           uint8_t* buffer, uint32_t bufsize) {
  if (!gTargetReady) {
    tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
    return TUD_MSC_RET_ERROR;
  }

  int state = ioLoad();
  switch (state) {
    case IO_IDLE: {
      uint32_t n = gBlkSize < bufsize ? gBlkSize : bufsize;
      memcpy(gIoBuf, buffer, n);
      gIoIsWrite = true;
      gIoLba = lba + offset / gBlkSize;
      gIoCount = 1;
      ioStore(IO_REQ);
      return TUD_MSC_RET_BUSY;
    }
    case IO_DONE: {
      uint32_t n = gBlkSize < bufsize ? gBlkSize : bufsize;
      ioStore(IO_IDLE);
      return (int32_t)n;
    }
    case IO_ERR: {
      ioStore(IO_IDLE);
      // write error
      tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x0C, 0x00);
      return TUD_MSC_RET_ERROR;
    }
    default:  // IO_REQ / IO_INFLIGHT
      return TUD_MSC_RET_BUSY;
  }
}

// Other SCSI commands. Unsupported ones fail with ILLEGAL REQUEST.
int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void* buffer,
                        uint16_t bufsize) {
  (void)buffer;
  (void)bufsize;
  switch (scsi_cmd[0]) {
    case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
      return 0;  // allow the host to proceed
    default:
      tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
      return TUD_MSC_RET_ERROR;
  }
}

}  // extern "C"

#endif  // TESTRIG_MSC_PASSTHROUGH
