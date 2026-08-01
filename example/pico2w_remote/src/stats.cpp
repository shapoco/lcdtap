#include "stats.hpp"

#include "hardware/pio.h"

#include "config.h"

// =============================================================================
// Module-level state
// =============================================================================

static StatsSources gSrc = {};

// Monotonic raw counters sampled from all sources. "Reset" keeps a baseline
// copy and displays current - baseline (wrap-safe uint32 subtraction).
struct RawCounters {
  uint32_t rxCmdBytes;
  uint32_t rxDataBytes;
  uint32_t rxDropBytes;
  uint32_t rxHwOverflow;
  uint32_t hwResets;
  uint32_t unknownCmds;
};

static RawCounters gBase = {};

// PIO RXSTALL sticky-flag observations (SPI/parallel input path)
static uint32_t gPioStallCount = 0;

// RX rate, updated once per second from the byte counter delta
static uint32_t gRateBps = 0;
static uint32_t gPrevTotalBytes = 0;
static uint64_t gLastRateMs = 0;
static uint64_t gNowMs = 0;

// HEX-format sentinel: values above 0xFF render as "--" (none yet)
static constexpr uint32_t HEX_NONE = 0x100u;

// =============================================================================
// Raw counter sampling
// =============================================================================

static void sampleRaw(RawCounters* c) {
  c->rxCmdBytes = gSrc.lcdtap->getRxCmdBytes();
  c->rxDataBytes = gSrc.lcdtap->getRxDataBytes();
  // SPI and I2C are never active at the same time, so a plain sum is exact.
  // Ring words carry one byte each, so words == bytes.
  c->rxDropBytes = gSrc.spi->dropWords + gSrc.i2c->dropWords;
  c->rxHwOverflow = gPioStallCount + gSrc.i2c->hwOverflowCount;
  c->hwResets = gSrc.lcdtap->getHwResetCount();
  c->unknownCmds = gSrc.lcdtap->getUnknownCmdCount();
}

// =============================================================================
// Public API
// =============================================================================

void statsInit(const StatsSources& src) {
  gSrc = src;
  gPioStallCount = 0;
  gRateBps = 0;
  gPrevTotalBytes = 0;
  gLastRateMs = 0;
  gNowMs = 0;
  sampleRaw(&gBase);
}

void statsTick(uint64_t nowMs) {
  gNowMs = nowMs;
  if (nowMs - gLastRateMs < 1000u) return;

  // PIO RXSTALL: the input SM stalled on a full RX FIFO, i.e. the DMA fell
  // behind the bus and input bits were lost before the ring buffer. Sticky
  // flag, write-1-to-clear; polled at 1 Hz.
  if (*gSrc.currentIface != lcdtap::BusType::I2C) {
    const uint32_t bit = 1u << (PIO_FDEBUG_RXSTALL_LSB + SPI_SM);
    if (SPI_PIO->fdebug & bit) {
      SPI_PIO->fdebug = bit;
      ++gPioStallCount;
    }
  }

  const uint32_t total =
      gSrc.lcdtap->getRxCmdBytes() + gSrc.lcdtap->getRxDataBytes();
  const uint32_t deltaBytes = total - gPrevTotalBytes;  // wrap-safe
  const uint32_t elapsedMs = static_cast<uint32_t>(nowMs - gLastRateMs);
  if (gLastRateMs != 0u && elapsedMs != 0u) {
    gRateBps = static_cast<uint32_t>(static_cast<uint64_t>(deltaBytes) * 1000u /
                                     elapsedMs);
  }
  gPrevTotalBytes = total;
  gLastRateMs = nowMs;
}

int statsCollect(lcdtap::StatEntry* out, int maxCount) {
  using lcdtap::StatFormat;

  RawCounters c;
  sampleRaw(&c);

  const uint32_t peakBacklog =
      (gSrc.spi->backlogMaxWords > gSrc.i2c->backlogMaxWords)
          ? gSrc.spi->backlogMaxWords
          : gSrc.i2c->backlogMaxWords;
  const uint32_t lastUnk = (c.unknownCmds == gBase.unknownCmds)
                               ? HEX_NONE
                               : gSrc.lcdtap->getLastUnknownCmd();

  const lcdtap::StatEntry entries[] = {
      {"RX Command Bytes", c.rxCmdBytes - gBase.rxCmdBytes, "B",
       StatFormat::DEC},
      {"RX Data Bytes", c.rxDataBytes - gBase.rxDataBytes, "B",
       StatFormat::DEC},
      {"RX Rate", gRateBps, "", StatFormat::RATE},
      {"RX Drop", c.rxDropBytes - gBase.rxDropBytes, "B", StatFormat::DEC},
      {"RX Peak Backlog", peakBacklog, "B", StatFormat::DEC},
      {"RX HW Overflow", c.rxHwOverflow - gBase.rxHwOverflow, "",
       StatFormat::DEC},
      {"HW Resets", c.hwResets - gBase.hwResets, "", StatFormat::DEC},
      {"Unknown Commands", c.unknownCmds - gBase.unknownCmds, "",
       StatFormat::DEC},
      {"Last Unknown Command", lastUnk, "", StatFormat::HEX},
      {"Uptime", static_cast<uint32_t>(gNowMs / 1000u), "s", StatFormat::DEC},
  };

  const int n = static_cast<int>(sizeof(entries) / sizeof(entries[0]));
  int count = (maxCount < n) ? maxCount : n;
  if (count < 0) count = 0;
  for (int i = 0; i < count; ++i) out[i] = entries[i];
  return count;
}

void statsReset() {
  sampleRaw(&gBase);
  // High-water marks are not monotonic, so baseline subtraction does not
  // apply. Both fields are only ever written from the Core 1 drain loop;
  // clearing them from Core 0 can lose at most one concurrent update, which
  // is acceptable for a debug counter.
  gSrc.spi->backlogMaxWords = 0;
  gSrc.i2c->backlogMaxWords = 0;
}
