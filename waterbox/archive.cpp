// See archive.h. A zip reader that never unpacks: the central directory is
// read once, and each open file pulls its own bytes out where they lie.
// SPDX-License-Identifier: MIT
#include "archive.h"

#include <algorithm>
#include <cstring>

#include <zlib.h>

namespace chimera
{
  namespace
  {
    u16 rd16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }
    u32 rd32(const u8* p) { return static_cast<u32>(p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<u32>(p[3]) << 24)); }
    u64 rd64(const u8* p) { return static_cast<u64>(rd32(p)) | (static_cast<u64>(rd32(p + 4)) << 32); }

    constexpr u32 SIG_EOCD = 0x06054b50;
    constexpr u32 SIG_EOCD64 = 0x06064b50;
    constexpr u32 SIG_LOC64 = 0x07064b50;
    constexpr u32 SIG_CDIR = 0x02014b50;
    constexpr u32 SIG_LOCAL = 0x04034b50;

    bool read_exact(std::FILE* f, u64 offset, void* buf, size_t n)
    {
      return std::fseek(f, static_cast<long>(offset), SEEK_SET) == 0 && std::fread(buf, 1, n, f) == n;
    }

    // The ZIP64 extra field replaces only the members that overflowed, in a
    // fixed order, so each is read only when its 32-bit field was saturated.
    void apply_zip64_extra(const u8* extra, size_t len, zip_entry& e, bool need_size, bool need_csize, bool need_offset)
    {
      size_t at = 0;
      while (at + 4 <= len)
      {
        const u16 id = rd16(extra + at);
        const u16 sz = rd16(extra + at + 2);
        if (at + 4 + sz > len)
          return;
        if (id == 0x0001)
        {
          size_t p = at + 4;
          const size_t end = p + sz;
          if (need_size && p + 8 <= end) { e.size = rd64(extra + p); p += 8; }
          if (need_csize && p + 8 <= end) { e.csize = rd64(extra + p); p += 8; }
          if (need_offset && p + 8 <= end) { e.header_offset = rd64(extra + p); p += 8; }
          return;
        }
        at += 4 + sz;
      }
    }
  }  // namespace

  bool zip_open(const std::string& host_path, zip_index& out, std::string& error)
  {
    std::FILE* f = std::fopen(host_path.c_str(), "rb");
    if (!f)
    {
      error = "cannot open " + host_path;
      return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long file_size_l = std::ftell(f);
    if (file_size_l <= 0)
    {
      std::fclose(f);
      error = "the archive is empty";
      return false;
    }
    const u64 file_size = static_cast<u64>(file_size_l);

    // The end-of-central-directory record is last, behind a comment of up to
    // 64 KiB, so it is found by scanning back rather than by seeking to it.
    const u64 tail_len = std::min<u64>(file_size, 66u * 1024u);
    std::vector<u8> tail(static_cast<size_t>(tail_len));
    if (!read_exact(f, file_size - tail_len, tail.data(), tail.size()))
    {
      std::fclose(f);
      error = "cannot read the end of the archive";
      return false;
    }
    size_t eocd = tail.size();
    for (size_t i = tail.size() >= 22 ? tail.size() - 22 : 0; ; i--)
    {
      if (rd32(tail.data() + i) == SIG_EOCD)
      {
        eocd = i;
        break;
      }
      if (i == 0)
        break;
    }
    if (eocd == tail.size())
    {
      std::fclose(f);
      error = "not a zip archive (no end-of-central-directory record)";
      return false;
    }

    u64 count = rd16(tail.data() + eocd + 10);
    u64 cd_size = rd32(tail.data() + eocd + 12);
    u64 cd_offset = rd32(tail.data() + eocd + 16);

    // ZIP64: a disc-sized archive always needs it, and its locator sits
    // immediately before the record just found.
    if (eocd >= 20 && rd32(tail.data() + eocd - 20) == SIG_LOC64)
    {
      const u64 rec_at = rd64(tail.data() + eocd - 20 + 8);
      u8 rec[56];
      if (read_exact(f, rec_at, rec, sizeof rec) && rd32(rec) == SIG_EOCD64)
      {
        count = rd64(rec + 32);
        cd_size = rd64(rec + 40);
        cd_offset = rd64(rec + 48);
      }
    }

    if (cd_size == 0 || cd_offset + cd_size > file_size)
    {
      std::fclose(f);
      error = "the archive's central directory is out of range";
      return false;
    }
    std::vector<u8> cd(static_cast<size_t>(cd_size));
    if (!read_exact(f, cd_offset, cd.data(), cd.size()))
    {
      std::fclose(f);
      error = "cannot read the archive's central directory";
      return false;
    }
    std::fclose(f);

    out.host_path = host_path;
    out.entries.clear();
    out.entries.reserve(static_cast<size_t>(count));

    size_t at = 0;
    while (at + 46 <= cd.size() && rd32(cd.data() + at) == SIG_CDIR)
    {
      zip_entry e;
      e.method = rd16(cd.data() + at + 10);
      e.csize = rd32(cd.data() + at + 20);
      e.size = rd32(cd.data() + at + 24);
      const u16 name_len = rd16(cd.data() + at + 28);
      const u16 extra_len = rd16(cd.data() + at + 30);
      const u16 comment_len = rd16(cd.data() + at + 32);
      e.header_offset = rd32(cd.data() + at + 42);
      if (at + 46 + name_len + extra_len + comment_len > cd.size())
        break;
      e.path.assign(reinterpret_cast<const char*>(cd.data() + at + 46), name_len);
      apply_zip64_extra(cd.data() + at + 46 + name_len, extra_len, e,
                        e.size == 0xFFFFFFFFu, e.csize == 0xFFFFFFFFu, e.header_offset == 0xFFFFFFFFu);
      at += 46 + name_len + extra_len + comment_len;

      for (char& c : e.path)
        if (c == '\\')
          c = '/';
      while (!e.path.empty() && e.path.front() == '/')
        e.path.erase(e.path.begin());
      // directory entries carry no bytes; the tree is built from the paths
      if (e.path.empty() || e.path.back() == '/')
        continue;
      if (e.method != 0 && e.method != 8)
      {
        error = "the archive uses a compression method this core cannot read (entry " + e.path + ")";
        return false;
      }
      out.entries.push_back(std::move(e));
    }

    if (out.entries.empty())
    {
      error = "the archive holds no files";
      return false;
    }
    return true;
  }

  zip_stream::zip_stream(std::shared_ptr<const zip_index> index, size_t entry)
    : m_index(std::move(index)), m_entry(entry)
  {
    m_file = std::fopen(m_index->host_path.c_str(), "rb");
  }

  zip_stream::~zip_stream()
  {
    if (m_stream)
    {
      inflateEnd(static_cast<z_stream*>(m_stream));
      delete static_cast<z_stream*>(m_stream);
    }
    if (m_file)
      std::fclose(m_file);
  }

  // The local header repeats the name and carries its own extra field, so
  // where an entry's bytes actually start is only known from the file.
  bool zip_stream::ensure_data_offset()
  {
    if (m_resolved)
      return true;
    if (!m_file)
      return false;
    const zip_entry& e = m_index->entries[m_entry];
    u8 hdr[30];
    if (!read_exact(m_file, e.header_offset, hdr, sizeof hdr) || rd32(hdr) != SIG_LOCAL)
      return false;
    m_data_offset = e.header_offset + 30 + rd16(hdr + 26) + rd16(hdr + 28);
    m_resolved = true;
    return true;
  }

  bool zip_stream::restart()
  {
    const zip_entry& e = m_index->entries[m_entry];
    if (!m_stream)
      m_stream = new z_stream{};
    z_stream* zs = static_cast<z_stream*>(m_stream);
    if (m_pos || zs->next_in)
      inflateEnd(zs);
    std::memset(zs, 0, sizeof(z_stream));
    // raw deflate: a zip member has no zlib header
    if (inflateInit2(zs, -MAX_WBITS) != Z_OK)
      return false;
    if (m_in.empty())
      m_in.resize(64 * 1024);
    if (m_skip.empty())
      m_skip.resize(64 * 1024);
    m_pos = 0;
    m_in_left = e.csize;
    return std::fseek(m_file, static_cast<long>(m_data_offset), SEEK_SET) == 0;
  }

  bool zip_stream::skip_to(u64 offset)
  {
    while (m_pos < offset)
    {
      const u64 want = std::min<u64>(m_skip.size(), offset - m_pos);
      const u64 got = read_at(m_pos, m_skip.data(), want);
      if (got == 0)
        return false;
    }
    return true;
  }

  u64 zip_stream::read_at(u64 offset, void* buffer, u64 size)
  {
    if (!m_file || !ensure_data_offset())
      return 0;
    const zip_entry& e = m_index->entries[m_entry];
    if (offset >= e.size || size == 0)
      return 0;
    size = std::min<u64>(size, e.size - offset);

    if (e.method == 0)
    {
      if (std::fseek(m_file, static_cast<long>(m_data_offset + offset), SEEK_SET) != 0)
        return 0;
      return std::fread(buffer, 1, static_cast<size_t>(size), m_file);
    }

    // Deflate has no index: reaching a position means decompressing up to it.
    // Reads that walk forward - which is how a game reads its data - carry on
    // from the cursor; a read that goes backwards starts the entry again.
    if (!m_stream || offset < m_pos)
    {
      if (!restart())
        return 0;
    }
    if (offset > m_pos && !skip_to(offset))
      return 0;

    z_stream* zs = static_cast<z_stream*>(m_stream);
    zs->next_out = static_cast<Bytef*>(buffer);
    zs->avail_out = static_cast<uInt>(std::min<u64>(size, 0xFFFFFFFFu));
    const uInt wanted = zs->avail_out;

    while (zs->avail_out)
    {
      if (zs->avail_in == 0 && m_in_left)
      {
        const size_t want = static_cast<size_t>(std::min<u64>(m_in.size(), m_in_left));
        const size_t got = std::fread(m_in.data(), 1, want, m_file);
        if (got == 0)
          break;
        m_in_left -= got;
        zs->next_in = m_in.data();
        zs->avail_in = static_cast<uInt>(got);
      }
      const int rc = inflate(zs, Z_NO_FLUSH);
      if (rc == Z_STREAM_END)
        break;
      if (rc != Z_OK)
        break;
      if (zs->avail_in == 0 && m_in_left == 0)
        break;
    }

    const u64 produced = wanted - zs->avail_out;
    m_pos += produced;
    return produced;
  }
}  // namespace chimera
