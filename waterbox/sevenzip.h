// A read-only view of a .7z archive that is never unpacked.
//
// The same bargain as archive.h makes for zip, and for the same reason: a
// PlayStation 3 game dumped as a folder is thousands of files and tens of
// gigabytes, a chimera project carries FILES rather than directory trees, and
// the sandbox has nowhere to put an unpacked disc. Entries stay where they are
// and bytes come out as the machine asks for them.
//
// WHAT IS DIFFERENT FROM ZIP, and it decides how this performs. A zip member is
// its own compressed stream, so every entry starts at a known offset. A .7z
// stores FOLDERS - solid blocks - and a folder holds one compressed stream that
// every file in it shares. Reaching file n in a folder means decoding files
// 0..n-1 first, because LZMA has no index and the dictionary carries across the
// boundary.
//
// So this reads a folder the only way a folder can be read: one forward cursor
// per open file, decoding from the start of its folder and discarding what
// comes before it. That is cheap when a folder holds one file (7-Zip's
// `-ms=off`) and hopeless when it holds a thousand - measured on a real dump,
// 190 seconds to reach an EBOOT.BIN packed 7 GB into its block, and again for
// every seek backwards. sz_open reports which kind it is in `solid`, and the
// driver refuses the solid kind at load: correct but unplayable is worse than
// refused, because it looks like a hang rather than a wait.
//
// Only what the SDK can decode incrementally is accepted: a folder is one coder
// and that coder is Copy, LZMA or LZMA2. A chain (BCJ2 and friends) needs four
// streams decoded in lockstep with nowhere to put the intermediates, so it is
// refused by name at open rather than part way through a game.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "util/types.hpp"

namespace chimera
{
  struct sz_entry
  {
    std::string path;         // as stored, '/' separated, never with a leading '/'
    u64 size = 0;             // uncompressed
    u32 folder = 0;           // which solid block it lives in
    u64 offset_in_folder = 0; // where its bytes start in that block's output
    u32 crc = 0;              // as stored, when the archive carries one
    bool has_crc = false;
  };

  // One solid block, reduced to what reading it needs. Everything the SDK's
  // header parser knows is copied out here at open, so a read never touches
  // CSzArEx again and the SDK's headers stay out of every translation unit
  // that merely wants bytes.
  struct sz_folder
  {
    u32 method = 0;        // 0 Copy, 0x21 LZMA2, 0x030101 LZMA
    std::vector<u8> props; // the coder's properties, as stored
    u64 pack_pos = 0;      // compressed bytes, from the start of the file
    u64 pack_size = 0;
    u64 unpack_size = 0;
  };

  // The header, read once. Immutable afterwards, so any number of open files
  // may share one index.
  struct sz_index
  {
    std::string host_path;
    std::vector<sz_entry> entries;
    std::vector<sz_folder> folders;

    // True when any folder holds more than one file, which is 7-Zip's default.
    // Reads are still correct; they are just paid for in decoding, so the
    // driver says so at load time and names the flag that avoids it.
    bool solid = false;
  };

  // Reads the header. False when the file is not a .7z, or is one this cannot
  // decode incrementally; `error` then says which, in words a person can act
  // on.
  bool sz_open(const std::string& host_path, sz_index& out, std::string& error);

  // One open file inside the archive. Each holds its own handle and its own
  // decoder, so reads of different entries never disturb each other and nothing
  // needs a lock: a green thread that yields mid-read resumes on its own
  // cursor.
  class sz_stream
  {
  public:
    sz_stream(std::shared_ptr<const sz_index> index, size_t entry);
    ~sz_stream();

    sz_stream(const sz_stream&) = delete;
    sz_stream& operator=(const sz_stream&) = delete;

    // Bytes of the UNCOMPRESSED entry at `offset`. Short reads mean the end.
    u64 read_at(u64 offset, void* buffer, u64 size);

  private:
    bool restart();                 // back to the first byte of the folder
    bool skip_to(u64 folder_pos);   // decode forward, discarding
    u64 decode_into(void* buffer, u64 size);  // the folder's next bytes, from the cursor

    std::shared_ptr<const sz_index> m_index;
    size_t m_entry = 0;
    std::FILE* m_file = nullptr;

    // the folder's compressed extent, resolved once from the header
    u64 m_pack_pos = 0;
    u64 m_pack_size = 0;
    u32 m_method = 0;
    bool m_resolved = false;

    // the cursor, in the FOLDER's uncompressed stream (not the entry's)
    void* m_dec = nullptr;   // CLzmaDec or CLzma2Dec, owned
    u64 m_pos = 0;
    u64 m_in_left = 0;      // compressed bytes of the folder not yet read from the file
    std::vector<u8> m_in;   // compressed input window
    size_t m_in_pos = 0;    // how much of that window the decoder has taken
    size_t m_in_have = 0;
    std::vector<u8> m_skip; // scratch for seeking forward
  };
}  // namespace chimera
