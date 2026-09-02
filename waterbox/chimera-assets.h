// Data files the emulator's renderer needs at run time, carried inside the
// core (gen-assets.py). Installed into the memory filesystem at boot.
// SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>
#include <cstdint>

struct chimera_asset
{
  const char* path;  // relative to the emulator's config dir
  const uint8_t* data;
  size_t size;
};

extern const chimera_asset chimera_assets[];
extern const size_t chimera_asset_count;
