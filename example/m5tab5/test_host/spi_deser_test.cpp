// Host-side unit test for spi_deser.hpp (no MCU dependencies).
//
// Verifies the word-at-a-time bulk decoder against a straightforward
// per-sample reference model over randomized CS/DC/MOSI streams, random
// chunk splits, misaligned buffer offsets, and small sink capacities.
//
// Build & run:
//   g++ -O2 -Wall -Wextra -I../include -o /tmp/spi_deser_test
//   spi_deser_test.cpp /tmp/spi_deser_test

#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "lcdtap/m5tab5/spi_deser.hpp"

using namespace lcdtap::m5tab5;

struct Event {
  bool isCommand;
  uint8_t byte;
  bool operator==(const Event &o) const {
    return isCommand == o.isCommand && byte == o.byte;
  }
};

// One capture sample (one SCK rising edge).
struct Sample {
  bool mosi;
  bool dc;
  bool cs;  // wire level: true = idle (deasserted)
};

// Reference model: literal transcription of the per-sample semantics
// (identical to the original pre-optimization decoder).
static void referenceDecode(const std::vector<Sample> &samples,
                            bool discardUntilIdle, std::vector<Event> *out) {
  uint8_t curByte = 0;
  uint8_t bitCount = 0;
  bool inFrame = false;
  for (const Sample &s : samples) {
    if (s.cs) {
      curByte = 0;
      bitCount = 0;
      inFrame = false;
      discardUntilIdle = false;
      continue;
    }
    inFrame = true;
    (void)inFrame;
    curByte = (uint8_t)((curByte << 1) | (s.mosi ? 1u : 0u));
    if (++bitCount == 8) {
      if (!discardUntilIdle) out->push_back({!s.dc, curByte});
      curByte = 0;
      bitCount = 0;
    }
  }
}

// Sink recording a flattened event stream.
static std::vector<Event> gEvents;
static void sinkDataFull(void *user) {
  SpiDeserSink *sink = static_cast<SpiDeserSink *>(user);
  for (uint32_t i = 0; i < sink->dataLen; ++i) {
    gEvents.push_back({false, sink->dataBuf[i]});
  }
}
static void sinkCommand(void *, uint8_t byte) {
  gEvents.push_back({true, byte});
}

// Pack samples into the raw PARLIO byte stream (2 samples/byte, earlier
// sample in the high nibble; lane3 mirrors CS as on the real wiring).
static std::vector<uint8_t> packSamples(const std::vector<Sample> &samples) {
  std::vector<uint8_t> raw;
  for (size_t i = 0; i + 1 < samples.size(); i += 2) {
    auto nib = [](const Sample &s) -> uint8_t {
      return (uint8_t)((s.cs ? 0x0Cu : 0u) | (s.dc ? 0x02u : 0u) |
                       (s.mosi ? 0x01u : 0u));
    };
    raw.push_back((uint8_t)((nib(samples[i]) << 4) | nib(samples[i + 1])));
  }
  return raw;
}

static int gFailures = 0;

static void runCase(std::mt19937 &rng, const std::vector<Sample> &samples,
                    bool discardUntilIdle, uint32_t dataCap, int alignOffset,
                    const char *label) {
  std::vector<Event> expected;
  referenceDecode(samples, discardUntilIdle, &expected);

  std::vector<uint8_t> raw = packSamples(samples);
  // Place the stream at a controlled misalignment inside a padded buffer.
  std::vector<uint8_t> buf(raw.size() + 8, 0xEE);
  std::copy(raw.begin(), raw.end(), buf.begin() + alignOffset);

  std::vector<uint8_t> staging(dataCap);
  SpiDeserSink sink = {};
  sink.dataBuf = staging.data();
  sink.dataCap = dataCap;
  sink.dataLen = 0;
  sink.onDataFull = sinkDataFull;
  sink.onCommand = sinkCommand;
  sink.user = &sink;

  SpiDeser d;
  spiDeserInit(&d);
  d.discardUntilIdle = discardUntilIdle;

  gEvents.clear();
  uint32_t pos = 0;
  std::uniform_int_distribution<uint32_t> chunkDist(1, 97);
  while (pos < raw.size()) {
    uint32_t n = std::min<uint32_t>(chunkDist(rng), raw.size() - pos);
    spiDeserProcess(&d, buf.data() + alignOffset + pos, n, &sink);
    pos += n;
  }
  spiDeserFlushData(&sink);  // final partial run, as the driver does

  if (gEvents.size() != expected.size() ||
      !std::equal(gEvents.begin(), gEvents.end(), expected.begin())) {
    ++gFailures;
    printf("FAIL [%s] cap=%u off=%d discard=%d: got %zu events, want %zu\n",
           label, dataCap, alignOffset, (int)discardUntilIdle, gEvents.size(),
           expected.size());
    size_t n = std::min(gEvents.size(), expected.size());
    for (size_t i = 0; i < n; ++i) {
      if (!(gEvents[i] == expected[i])) {
        printf("  first mismatch at %zu: got %c%02X want %c%02X\n", i,
               gEvents[i].isCommand ? 'C' : 'D', gEvents[i].byte,
               expected[i].isCommand ? 'C' : 'D', expected[i].byte);
        break;
      }
    }
  }
}

// Random stream: alternating CS-idle gaps and CS-active frames with
// random lengths (often not multiples of 8 bits) and random MOSI/DC.
static std::vector<Sample> randomStream(std::mt19937 &rng, bool startIdle) {
  std::vector<Sample> s;
  std::uniform_int_distribution<int> gapDist(0, 5);
  std::uniform_int_distribution<int> frameDist(1, 300);
  std::uniform_int_distribution<int> bitDist(0, 1);
  std::uniform_int_distribution<int> dcModeDist(0, 2);
  int numFrames = 1 + (int)(rng() % 8);
  for (int f = 0; f < numFrames; ++f) {
    int gap = gapDist(rng);
    if (f == 0 && !startIdle) gap = 0;
    for (int i = 0; i < gap; ++i) s.push_back({(bool)bitDist(rng), true, true});
    int frameLen = frameDist(rng);
    int dcMode = dcModeDist(rng);  // 0: all data, 1: all cmd, 2: random
    for (int i = 0; i < frameLen; ++i) {
      bool dc = dcMode == 0 ? true : (dcMode == 1 ? false : bitDist(rng));
      s.push_back({(bool)bitDist(rng), dc, false});
    }
  }
  if (s.size() & 1) s.push_back({false, false, true});  // pad to whole bytes
  return s;
}

int main() {
  std::mt19937 rng(0xC0FFEE);

  // Deterministic vector from the header doc: 0xA5 as a command byte.
  {
    std::vector<Sample> s;
    s.push_back({false, false, true});
    s.push_back({false, false, true});
    uint8_t val = 0xA5;
    for (int i = 7; i >= 0; --i) {
      s.push_back({((val >> i) & 1) != 0, false, false});
    }
    s.push_back({false, false, true});
    s.push_back({false, false, true});
    runCase(rng, s, false, 4, 0, "doc-0xA5-cmd");
  }

  // Small caps exercise the flush boundaries: 1/3 keep the quad path
  // disabled (dataCap < 4), 4/5/7 hit the quad pre/post-flush edges.
  const uint32_t caps[] = {1, 3, 4, 5, 7, 4096};
  for (int iter = 0; iter < 500; ++iter) {
    std::vector<Sample> s = randomStream(rng, (iter & 1) != 0);
    for (uint32_t cap : caps) {
      for (int off = 0; off < 4; ++off) {
        runCase(rng, s, false, cap, off, "random");
        runCase(rng, s, true, cap, off, "random-discard");
      }
    }
  }

  if (gFailures) {
    printf("%d failure(s)\n", gFailures);
    return 1;
  }
  printf("all tests passed\n");
  return 0;
}
