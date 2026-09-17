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

// The console's system messages in English, indexed by localized_string_id's
// value (generated from rpcs3's own table, see gen-assets.py). A "%0" in one is
// where the argument goes.
extern const char* const chimera_localized_text[];
extern const size_t chimera_localized_text_count;
