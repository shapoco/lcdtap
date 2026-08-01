#pragma once

// Embedded static web assets, generated at build time by
// tools/embed_assets.py from the shared files under docs/.

#include <cstdint>

struct WebAsset {
  const char* path;     // request path, e.g. "/" or "/monitor.js"
  const char* mime;     // Content-Type value
  const uint8_t* data;  // gzip-compressed body (flash-resident)
  uint32_t len;
  uint8_t gzip;  // 1 = serve with Content-Encoding: gzip
};

extern const WebAsset WEB_ASSETS[];
extern const int WEB_ASSET_COUNT;
