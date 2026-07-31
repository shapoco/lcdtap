#pragma once

#include <cstdint>

// Encode exactly 3 bytes to 4 Base64 characters.
void b64Encode3(const uint8_t in[3], char out[4]);

// Encode 1 or 2 bytes with '=' padding to 4 Base64 characters.
void b64EncodePad(const uint8_t* in, int count, char out[4]);
