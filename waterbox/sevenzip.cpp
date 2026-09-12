// SPDX-License-Identifier: MIT
#include "sevenzip.h"

#include <algorithm>
#include <cstring>

extern "C"
{
#include "7z.h"
#include "7zAlloc.h"
#include "7zCrc.h"
#include "Lzma2Dec.h"
#include "LzmaDec.h"
}

namespace chimera
{
  namespace
  {
    constexpr u32 kCopy = 0x00;
    constexpr u32 kLzma = 0x030101;
    constexpr u32 kLzma2 = 0x21;

    ISzAlloc g_alloc = {SzAlloc, SzFree};

    // An ISeekInStream over a plain FILE*, so that none of the SDK's own file
    // code - which is POSIX or Win32 by #ifdef, and neither of those is what a
    // guest has - is compiled in. fopen is what archive.cpp already reads zips
    // through, and it reaches the same mounted host file.
    struct file_stream
    {
      ISeekInStream vt;
      std::FILE* f;
    };

    file_stream* self_of(ISeekInStreamPtr pp)
    {
      return const_cast<file_stream*>(reinterpret_cast<const file_stream*>(pp));
    }

    SRes fs_read(ISeekInStreamPtr pp, void* buf, size_t* size)
    {
      if (*size == 0)
        return SZ_OK;
      const size_t got = std::fread(buf, 1, *size, self_of(pp)->f);
      *size = got;
      // A short read is the end of the file, which the SDK reads as such; only
      // a read that fails outright is an error.
      return (got || std::feof(self_of(pp)->f)) ? SZ_OK : SZ_ERROR_READ;
    }

    SRes fs_seek(ISeekInStreamPtr pp, Int64* pos, ESzSeek origin)
    {
      int whence = SEEK_SET;
      if (origin == SZ_SEEK_CUR)
        whence = SEEK_CUR;
      else if (origin == SZ_SEEK_END)
        whence = SEEK_END;
      std::FILE* f = self_of(pp)->f;
      if (std::fseek(f, static_cast<long>(*pos), whence) != 0)
        return SZ_ERROR_READ;
      const long now = std::ftell(f);
      if (now < 0)
        return SZ_ERROR_READ;
      *pos = now;
      return SZ_OK;
    }

    // 7-Zip stores names as UTF-16; everything this core hands to memfs is
    // UTF-8. Surrogate pairs are joined rather than passed through, so a name
    // outside the BMP survives the trip.
    std::string utf16_to_utf8(const UInt16* s, size_t len)
    {
      std::string out;
      out.reserve(len);
      for (size_t i = 0; i < len; i++)
      {
        u32 cp = s[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < len && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF)
        {
          cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
          i++;
        }
        if (cp < 0x80)
        {
          out.push_back(static_cast<char>(cp));
        }
        else if (cp < 0x800)
        {
          out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
          out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else if (cp < 0x10000)
        {
          out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
          out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
          out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else
        {
          out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
          out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
          out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
          out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
      }
      return out;
    }

    std::string method_name(u32 id)
    {
      char buf[32];
      std::snprintf(buf, sizeof buf, "0x%06X", id);
      return buf;
    }
  }  // namespace

  bool sz_open(const std::string& host_path, sz_index& out, std::string& error)
  {
    // The SDK's CRC table is global and built once; the header check needs it.
    static bool crc_ready = false;
    if (!crc_ready)
    {
      CrcGenerateTable();
      crc_ready = true;
    }

    std::FILE* f = std::fopen(host_path.c_str(), "rb");
    if (!f)
    {
      error = "cannot open " + host_path;
      return false;
    }

    file_stream fs{};
    fs.vt.Read = fs_read;
    fs.vt.Seek = fs_seek;
    fs.f = f;

    CLookToRead2 look;
    LookToRead2_CreateVTable(&look, False);
    look.realStream = &fs.vt;
    LookToRead2_INIT(&look)
    constexpr size_t kLookBuf = 1u << 16;
    look.buf = static_cast<Byte*>(ISzAlloc_Alloc(&g_alloc, kLookBuf));
    if (!look.buf)
    {
      std::fclose(f);
      error = "out of memory reading the archive header";
      return false;
    }
    look.bufSize = kLookBuf;

    CSzArEx db;
    SzArEx_Init(&db);

    bool ok = false;
    const SRes res = SzArEx_Open(&db, &look.vt, &g_alloc, &g_alloc);
    if (res != SZ_OK)
    {
      error = res == SZ_ERROR_NO_ARCHIVE  ? "this is not a .7z archive"
              : res == SZ_ERROR_UNSUPPORTED ? "this .7z uses something this core cannot read (encryption, most likely)"
              : res == SZ_ERROR_MEM         ? "out of memory reading the archive header"
              : res == SZ_ERROR_CRC         ? "the archive header is corrupt"
                                            : "the archive header could not be read";
    }
    else
    {
      out.host_path = host_path;
      out.entries.clear();
      out.folders.clear();
      out.solid = false;

      // Every folder, reduced to what a read needs. A folder this cannot decode
      // incrementally is refused HERE, by name, rather than part way through a
      // game that has already started.
      out.folders.resize(db.db.NumFolders);
      ok = true;
      for (u32 fi = 0; fi < db.db.NumFolders && ok; fi++)
      {
        CSzFolder folder;
        CSzData sd;
        sd.Data = db.db.CodersData + db.db.FoCodersOffsets[fi];
        sd.Size = db.db.FoCodersOffsets[fi + 1] - db.db.FoCodersOffsets[fi];
        if (SzGetNextFolderItem(&folder, &sd) != SZ_OK)
        {
          error = "the archive header is corrupt";
          ok = false;
          break;
        }

        if (folder.NumCoders != 1)
        {
          error =
            "this .7z was packed with a chain of coders (a filter such as BCJ2 in front of the "
            "compressor). Decoding one needs several streams advanced in lockstep, with the "
            "intermediates held somewhere, and the sandbox has nowhere to hold them. Repack with "
            "`7z a -m0=lzma2 -mf=off`, or as a .zip, and it will be read where it lies.";
          ok = false;
          break;
        }

        const CSzCoderInfo& coder = folder.Coders[0];
        sz_folder& dst = out.folders[fi];
        dst.method = static_cast<u32>(coder.MethodID);
        if (dst.method != kCopy && dst.method != kLzma && dst.method != kLzma2)
        {
          error = "this .7z uses compression method " + method_name(dst.method) +
                  ", which this core cannot decode. Repack with `7z a -m0=lzma2`, or as a .zip.";
          ok = false;
          break;
        }
        dst.props.assign(db.db.CodersData + coder.PropsOffset,
                         db.db.CodersData + coder.PropsOffset + coder.PropsSize);
        // SzArEx_GetFolderStreamPos and SzArEx_GetFolderFullPackSize are both
        // declared in 7z.h and defined NOWHERE in this SDK (26.02) - stale
        // declarations that only announce themselves at link time. Both come
        // from the pack table directly instead: dataPos is where packed data
        // begins, PackPositions are cumulative offsets from it, and a folder's
        // streams are contiguous.
        {
          const u32 first = db.db.FoStartPackStreamIndex[fi];
          const u32 last = db.db.FoStartPackStreamIndex[fi + 1];
          dst.pack_pos = db.dataPos + db.db.PackPositions[first];
          dst.pack_size = db.db.PackPositions[last] - db.db.PackPositions[first];
        }
        dst.unpack_size = SzAr_GetFolderUnpackSize(&db.db, fi);

        // 7-Zip packs solid by default: one block, every file in it. Correct to
        // read, dear to seek, so the caller is told rather than left to wonder
        // why a game takes its time.
        if (db.FolderToFile[fi + 1] - db.FolderToFile[fi] > 1)
          out.solid = true;
      }

      if (ok)
      {
        std::vector<UInt16> name;
        for (u32 i = 0; i < db.NumFiles; i++)
        {
          if (SzBitArray_Check(db.IsDirs, i))
            continue;
          const size_t chars = SzArEx_GetFileNameUtf16(&db, i, nullptr);
          name.resize(chars);
          SzArEx_GetFileNameUtf16(&db, i, name.data());
          // the SDK counts the terminator; the string does not want it
          std::string path = utf16_to_utf8(name.data(), chars ? chars - 1 : 0);
          std::replace(path.begin(), path.end(), '\\', '/');
          while (!path.empty() && path.front() == '/')
            path.erase(path.begin());
          if (path.empty())
            continue;

          sz_entry e;
          e.path = std::move(path);
          e.size = SzArEx_GetFileSize(&db, i);
          // 7-Zip stores a CRC32 per file. Nothing here checks it on the way
          // past - a disc is read in pieces and out of order, so there is no
          // moment when a whole file has gone by - but it is ground truth a
          // test can hold the reader to.
          if (SzBitWithVals_Check(&db.CRCs, i))
          {
            e.crc = db.CRCs.Vals[i];
            e.has_crc = true;
          }
          const u32 fi = db.FileToFolder[i];
          if (fi == static_cast<u32>(-1))
          {
            // an empty file belongs to no folder, and reading it returns nothing
            e.folder = 0;
            e.offset_in_folder = 0;
            e.size = 0;
          }
          else
          {
            e.folder = fi;
            e.offset_in_folder = db.UnpackPositions[i] - db.UnpackPositions[db.FolderToFile[fi]];
          }
          out.entries.push_back(std::move(e));
        }
      }
    }

    SzArEx_Free(&db, &g_alloc);
    ISzAlloc_Free(&g_alloc, look.buf);
    std::fclose(f);
    return ok;
  }

  sz_stream::sz_stream(std::shared_ptr<const sz_index> index, size_t entry)
    : m_index(std::move(index)), m_entry(entry)
  {
    m_file = std::fopen(m_index->host_path.c_str(), "rb");
  }

  sz_stream::~sz_stream()
  {
    if (m_dec)
    {
      if (m_method == kLzma2)
      {
        Lzma2Dec_Free(static_cast<CLzma2Dec*>(m_dec), &g_alloc);
        delete static_cast<CLzma2Dec*>(m_dec);
      }
      else
      {
        LzmaDec_Free(static_cast<CLzmaDec*>(m_dec), &g_alloc);
        delete static_cast<CLzmaDec*>(m_dec);
      }
    }
    if (m_file)
      std::fclose(m_file);
  }

  bool sz_stream::restart()
  {
    const sz_entry& e = m_index->entries[m_entry];
    if (e.folder >= m_index->folders.size())
      return false;
    const sz_folder& f = m_index->folders[e.folder];

    if (m_dec)
    {
      if (m_method == kLzma2)
      {
        Lzma2Dec_Free(static_cast<CLzma2Dec*>(m_dec), &g_alloc);
        delete static_cast<CLzma2Dec*>(m_dec);
      }
      else
      {
        LzmaDec_Free(static_cast<CLzmaDec*>(m_dec), &g_alloc);
        delete static_cast<CLzmaDec*>(m_dec);
      }
      m_dec = nullptr;
    }

    m_method = f.method;
    m_pack_pos = f.pack_pos;
    m_pack_size = f.pack_size;
    m_resolved = true;

    if (m_method == kLzma2)
    {
      if (f.props.size() != 1)
        return false;
      auto* dec = new CLzma2Dec();
      Lzma2Dec_Construct(dec);
      if (Lzma2Dec_Allocate(dec, f.props[0], &g_alloc) != SZ_OK)
      {
        delete dec;
        return false;
      }
      Lzma2Dec_Init(dec);
      m_dec = dec;
    }
    else if (m_method == kLzma)
    {
      auto* dec = new CLzmaDec();
      LzmaDec_Construct(dec);
      if (LzmaDec_Allocate(dec, f.props.data(), static_cast<unsigned>(f.props.size()), &g_alloc) != SZ_OK)
      {
        delete dec;
        return false;
      }
      LzmaDec_Init(dec);
      m_dec = dec;
    }

    if (m_in.empty())
      m_in.resize(64 * 1024);
    if (m_skip.empty())
      m_skip.resize(64 * 1024);
    m_in_pos = 0;
    m_in_have = 0;
    m_pos = 0;
    m_in_left = m_pack_size;
    return std::fseek(m_file, static_cast<long>(m_pack_pos), SEEK_SET) == 0;
  }

  bool sz_stream::skip_to(u64 folder_pos)
  {
    while (m_pos < folder_pos)
    {
      const u64 want = std::min<u64>(m_skip.size(), folder_pos - m_pos);
      // read_at works in ENTRY coordinates, so the skip is driven here in
      // folder coordinates instead: decode into the bin and drop it.
      const u64 got = decode_into(m_skip.data(), want);
      if (got == 0)
        return false;
    }
    return true;
  }

  // Decodes the next `size` bytes of the folder's stream, from wherever the
  // cursor is. Short returns mean the folder ended.
  u64 sz_stream::decode_into(void* buffer, u64 size)
  {
    u8* dest = static_cast<u8*>(buffer);
    u64 done = 0;
    while (done < size)
    {
      if (m_in_pos == m_in_have && m_in_left)
      {
        const size_t want = static_cast<size_t>(std::min<u64>(m_in.size(), m_in_left));
        const size_t got = std::fread(m_in.data(), 1, want, m_file);
        if (got == 0)
          break;
        m_in_left -= got;
        m_in_pos = 0;
        m_in_have = got;
      }

      SizeT dest_len = static_cast<SizeT>(size - done);
      SizeT src_len = static_cast<SizeT>(m_in_have - m_in_pos);
      ELzmaStatus status;
      SRes rc;
      if (m_method == kLzma2)
      {
        rc = Lzma2Dec_DecodeToBuf(static_cast<CLzma2Dec*>(m_dec), dest + done, &dest_len,
                                  m_in.data() + m_in_pos, &src_len, LZMA_FINISH_ANY, &status);
      }
      else
      {
        rc = LzmaDec_DecodeToBuf(static_cast<CLzmaDec*>(m_dec), dest + done, &dest_len,
                                 m_in.data() + m_in_pos, &src_len, LZMA_FINISH_ANY, &status);
      }
      if (rc != SZ_OK)
        break;
      m_in_pos += static_cast<size_t>(src_len);
      done += dest_len;
      m_pos += dest_len;
      if (dest_len == 0 && src_len == 0)
      {
        // nothing moved: either the stream is finished or there is no more
        // input to give it
        if (status == LZMA_STATUS_FINISHED_WITH_MARK || m_in_left == 0)
          break;
      }
    }
    return done;
  }

  u64 sz_stream::read_at(u64 offset, void* buffer, u64 size)
  {
    if (!m_file)
      return 0;
    const sz_entry& e = m_index->entries[m_entry];
    if (offset >= e.size || size == 0)
      return 0;
    size = std::min<u64>(size, e.size - offset);
    if (e.folder >= m_index->folders.size())
      return 0;
    const sz_folder& f = m_index->folders[e.folder];

    if (f.method == kCopy)
    {
      const u64 at = f.pack_pos + e.offset_in_folder + offset;
      if (std::fseek(m_file, static_cast<long>(at), SEEK_SET) != 0)
        return 0;
      return std::fread(buffer, 1, static_cast<size_t>(size), m_file);
    }

    // LZMA has no index: reaching a position means decoding up to it, and the
    // position is in the FOLDER's stream, which for a solid block starts before
    // this entry does. Forward reads - how a game reads its data - carry on
    // from the cursor; anything backwards starts the folder again.
    const u64 target = e.offset_in_folder + offset;
    if (!m_dec || target < m_pos)
    {
      if (!restart())
        return 0;
    }
    if (target > m_pos && !skip_to(target))
      return 0;
    return decode_into(buffer, size);
  }
}  // namespace chimera
