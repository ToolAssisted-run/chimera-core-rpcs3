// See memfs.h.
// SPDX-License-Identifier: MIT
#include "stdafx.h"

#include "Utilities/File.h"
#include "Loader/ISO.h"
#include "util/shared_ptr.hpp"
#include "vsched.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "memfs.h"

#include "archive.h"
#include "sevenzip.h"

namespace chimera
{
  namespace
  {
    struct node
    {
      bool dir = false;
      std::vector<u8> data;                              // memory file
      std::string host_path;                             // grafted file (read-only)
      u64 host_size = 0;
      // How the console reads this image, when reading it raw is not how: a
      // Redump dump's data regions are encrypted. Set by memfs_mark_disc_image
      // before the machine runs; plain data (an AES key schedule and a list of
      // regions), so a savestate carries it as it carries everything else here.
      std::shared_ptr<iso_file_decryption> disc_dec;
      std::shared_ptr<iso_file_decryption> disc_dec_off;  // held aside by memfs_set_disc_decryption
      bool disc_dec_tried = false;
      std::shared_ptr<const zip_index> arch;             // zip entry (read-only)
      size_t arch_entry = 0;
      std::shared_ptr<const sz_index> sz;                // .7z entry (read-only)
      size_t sz_entry = 0;
      std::map<std::string, std::shared_ptr<node>> children;
      s64 mtime = 0;
      // A MIRROR: a writable file whose bytes, so far, are exactly the first
      // mirrorLen bytes of a read-only file (a disc file), so those bytes are
      // not kept - see "not carrying the disc twice" below.
      std::shared_ptr<node> mirror;
      u64 mirrorLen = 0;
      u64 mirrorAt = 0;  // where in the source the mirror's first byte lies
    };

    bool readOnly(const node& n) { return !n.host_path.empty() || n.arch || n.sz; }
    u64 logical_size(const node& n) { return n.mirror ? n.mirrorLen : n.data.size(); }

    std::shared_ptr<node> g_root;
    // A file's mtime is the machine's own clock: the machine's calendar
    // (VSCHED_EPOCH_SECONDS, 2017-05-27) plus the virtual seconds the machine
    // has run - the same clock the console itself reads. A save made at frame
    // N carries the same date on every run; a counter would have shown every
    // save as made in January 1970.
    constexpr s64 kEpoch = static_cast<s64>(VSCHED_EPOCH_SECONDS);
    s64 now_mtime() { return kEpoch + static_cast<s64>(vsched_now_ns() / 1000000000ull); }

    std::vector<std::string> split(const std::string& rel)
    {
      std::vector<std::string> parts;
      std::string cur;
      for (char c : rel)
      {
        if (c == '/' || c == '\\')
        {
          if (!cur.empty() && cur != ".")
            parts.push_back(cur);
          cur.clear();
        }
        else
          cur += c;
      }
      if (!cur.empty() && cur != ".")
        parts.push_back(cur);
      return parts;
    }

    // The path as the device receives it carries the root prefix.
    std::string strip_root(const std::string& path)
    {
      if (path.compare(0, memfs_root.size(), memfs_root) == 0)
        return path.substr(memfs_root.size());
      return path;
    }

    std::shared_ptr<node> lookup(const std::string& rel, std::shared_ptr<node>* parent = nullptr, std::string* leaf = nullptr)
    {
      auto parts = split(rel);
      std::shared_ptr<node> cur = g_root;
      std::shared_ptr<node> par;
      for (size_t i = 0; i < parts.size(); i++)
      {
        if (!cur || !cur->dir)
          return nullptr;
        auto it = cur->children.find(parts[i]);
        par = cur;
        if (it == cur->children.end())
        {
          if (parent && i + 1 == parts.size())
          {
            *parent = par;
            if (leaf)
              *leaf = parts[i];
          }
          return nullptr;
        }
        cur = it->second;
        if (parent && i + 1 == parts.size())
        {
          *parent = par;
          if (leaf)
            *leaf = parts[i];
        }
      }
      if (parts.empty() && parent)
        *parent = nullptr;
      return cur;
    }

    std::shared_ptr<node> mkdirs(const std::string& rel)
    {
      std::shared_ptr<node> cur = g_root;
      for (auto& p : split(rel))
      {
        auto& child = cur->children[p];
        if (!child)
        {
          child = std::make_shared<node>();
          child->dir = true;
          child->mtime = now_mtime();
        }
        if (!child->dir)
          return nullptr;
        cur = child;
      }
      return cur;
    }

    void fill_stat(const node& n, fs::stat_t& st)
    {
      st.is_directory = n.dir;
      st.is_symlink = false;
      const bool in_memory = n.host_path.empty() && !n.arch && !n.sz;
      st.is_writable = in_memory;
      st.size = n.dir ? 0 : (in_memory ? logical_size(n) : n.host_size);
      st.atime = st.mtime = st.ctime = n.mtime;
    }

    // ---- not carrying the disc twice ------------------------------------------
    //
    // A game installs itself: Oblivion's caching thread reads 41 files, 4.30 GiB,
    // off the disc and writes them to the console's hard disk, which is this
    // filesystem, which is the machine's memory, which is every savestate. Four
    // fifths of a 5.45 GiB state were a second copy of files the project already
    // has in its ISO - already compressed by their authors, so zstd gave back
    // 1.27x and no more (chimera design log, 2026-09-17).
    //
    // The copy is the game's own loop of reads and writes through the kernel, so
    // this filesystem never sees "copy": it sees a read from a read-only file and,
    // a moment later, a write of the same bytes to a fresh one. The read-only
    // file is the ISO itself: rpcs3's ISO layer serves /dev_bdvd out of it with
    // read_at, so a disc file is a stretch of the ISO node and the game's file
    // offset 0 is that stretch's start. So this filesystem REMEMBERS the last
    // few reads from read-only files (source and offset). A big write into a
    // fresh empty file whose bytes equal a remembered read's start makes that
    // file a mirror of the source from that offset; a write at the end of a
    // mirror whose bytes equal the source at the matching offset extends it;
    // neither stores anything. The bytes are compared, not assumed: a mirror is
    // only ever a file whose contents are provably the source's. Any other write
    // - elsewhere in the file, with other bytes, a truncation - materialises the
    // file first (the source's stretch is copied in) and goes ahead as it always
    // did, so nothing a game can do reads differently.
    //
    // Deterministic: the decision depends only on the sequence of reads and
    // writes, which is the machine's, so native and sandbox agree.
    //
    // WHAT THE SOURCE HOLDS IS NOT ALWAYS WHAT IS IN THE FILE. A Redump dump of
    // a PS3 disc keeps the disc's filesystem in the clear and its DATA regions
    // encrypted; rpcs3's ISO layer decrypts them on the way past, so the bytes
    // the game reads - and writes to the hard disk - are nowhere in the image.
    // Compared raw, such a write matches nothing and every byte of the install
    // is kept: Resident Evil 5 Gold's own data install is several gigabytes,
    // which is more than an entire greenzone budget, so the game could not be
    // TASed at all. So a source that is a disc image is read the way the CONSOLE
    // reads it (read_through_disc below), and the comparison, the materialise
    // and the reads of the mirror itself all see the same bytes the machine
    // does. A decrypted image has no decryption and costs nothing extra.
    struct recent_read
    {
      std::weak_ptr<node> source;
      u64 offset = 0, size = 0;
    };
    constexpr unsigned RECENT_READS = 64;
    recent_read g_recentReads[RECENT_READS];
    unsigned g_recentNext = 0;
    // a write smaller than this starts no mirror: comparing every little write
    // against the disc is not worth what a little file weighs
    constexpr u64 MIRROR_MIN_START = 4096;

    void remember_read(const std::shared_ptr<node>& source, u64 offset, u64 size)
    {
      if (size == 0)
        return;
      g_recentReads[g_recentNext % RECENT_READS] = {source, offset, size};
      g_recentNext++;
    }

    // A read of a host file through a handle this filesystem keeps, with the
    // handle opened again when it has stopped being the file.
    //
    // It can stop being the file across a SAVESTATE. The FILE object lives in
    // the machine's memory and a savestate carries it faithfully, but the
    // descriptor inside it belongs to the sandbox, which keeps its open files
    // on the host side where no savestate reaches them (miniBox host.c: "Phase
    // 1 FS is fixed ... the memory block carries all mutable machine state").
    // Load a state and the machine is holding a number the sandbox has since
    // closed, or handed to another open of another file. The read fails, and a
    // game reading the data it installed onto the console's hard disk - which
    // this filesystem serves out of the disc image itself, as a mirror - is
    // told its own data is broken. Guilty Gear Xrd is told exactly that:
    //
    //   IO Failure detected with file /dev_hdd0/game/BLUS31588/USRDIR/FIOS-GGXRD/IS_DATA00.PSARC
    //   cellGameContentErrorDialog -> "ERROR: Game data is corrupted."
    //
    // and the machine stops there, on the character it was loading (#118).
    //
    // A SHORT read counts as failure: every caller asks for bytes the file is
    // known to hold, so less than that means the descriptor is not this file.
    // One reopen heals it, and the machine that comes out is frame-for-frame
    // the machine that never saved.
    u64 host_read_at(std::FILE*& f, const std::string& path, u64 host_size, u64 offset, void* buffer, u64 size)
    {
      if (offset >= host_size)
        return 0;
      const u64 want = std::min<u64>(size, host_size - offset);
      if (f != nullptr && std::fseek(f, static_cast<long>(offset), SEEK_SET) == 0)
      {
        const u64 got = std::fread(buffer, 1, static_cast<size_t>(want), f);
        if (got == want)
          return got;
      }
      if (f != nullptr)
        std::fclose(f);
      f = std::fopen(path.c_str(), "rb");
      if (f == nullptr || std::fseek(f, static_cast<long>(offset), SEEK_SET) != 0)
        return 0;
      return std::fread(buffer, 1, static_cast<size_t>(want), f);
    }

    memfs_mirror_stats g_mirrorStats;

    // Reads a read-only node without a file object of the game's: one reader per
    // source, kept, because a copy loop compares tens of thousands of chunks and
    // a reopen per chunk (or a restart of a compressed stream) would be its cost.
    struct source_reader
    {
      std::FILE* host = nullptr;
      std::unique_ptr<zip_stream> zip;
      std::unique_ptr<sz_stream> sevenz;
      const node* n = nullptr;
      std::vector<u8> sectors;  // a disc image's scratch: whole sectors, to decrypt in
      explicit source_reader(const node& src) : n(&src)
      {
        if (!src.host_path.empty())
          host = std::fopen(src.host_path.c_str(), "rb");
        else if (src.arch)
          zip = std::make_unique<zip_stream>(src.arch, src.arch_entry);
        else if (src.sz)
          sevenz = std::make_unique<sz_stream>(src.sz, src.sz_entry);
      }
      ~source_reader()
      {
        if (host)
          std::fclose(host);
      }
      u64 read_raw(u64 offset, void* buffer, u64 size)
      {
        return host_read_at(host, n->host_path, n->host_size, offset, buffer, size);
      }
      // The image as the CONSOLE reads it. Decryption is by sector and takes the
      // sector's number for its IV, so the window read is the whole sectors the
      // request falls in and the answer is the slice of them asked for.
      u64 read_through_disc(u64 offset, void* buffer, u64 size)
      {
        constexpr u64 kSector = 2048;  // ISO_SECTOR_SIZE
        if (offset >= n->host_size)
          return 0;
        size = std::min<u64>(size, n->host_size - offset);
        const u64 first = offset / kSector * kSector;
        const u64 last = std::min<u64>(n->host_size, (offset + size + kSector - 1) / kSector * kSector);
        if (sectors.size() < last - first)
          sectors.resize(static_cast<size_t>(last - first));
        const u64 got = read_raw(first, sectors.data(), last - first);
        if (got < offset - first + size)
          return 0;
        n->disc_dec->decrypt(first, std::span<u8>(sectors.data(), static_cast<size_t>(got)), n->host_path);
        g_mirrorStats.decryptedBytes += got;
        std::memcpy(buffer, sectors.data() + (offset - first), static_cast<size_t>(size));
        return size;
      }
      u64 read_at(u64 offset, void* buffer, u64 size)
      {
        if (zip)
          return zip->read_at(offset, buffer, size);
        if (sevenz)
          return sevenz->read_at(offset, buffer, size);
        // through the disc's own decryption when it has one, and either way
        // through host_read_at, so a handle a savestate broke is healed on the
        // path a mirror of an encrypted disc reads by as much as on any other
        if (n->disc_dec)
          return read_through_disc(offset, buffer, size);
        return read_raw(offset, buffer, size);
      }
    };
    std::map<const node*, std::unique_ptr<source_reader>> g_sourceReaders;

    source_reader& reader_for(const std::shared_ptr<node>& source)
    {
      auto& r = g_sourceReaders[source.get()];
      if (!r)
        r = std::make_unique<source_reader>(*source);
      return *r;
    }

    // Whether `buffer` is exactly what `source` holds at [offset, offset + size).
    bool source_holds(const std::shared_ptr<node>& source, u64 offset, const void* buffer, u64 size)
    {
      static std::vector<u8> scratch;
      if (scratch.size() < size)
        scratch.resize(static_cast<size_t>(size));
      if (reader_for(source).read_at(offset, scratch.data(), size) != size)
        return false;
      return std::memcmp(scratch.data(), buffer, static_cast<size_t>(size)) == 0;
    }

    // The source and offset, if any, of a recent read that begins with exactly
    // these bytes: the start of a disc file the game is copying out.
    //
    // How BIG that read was does not come into it. It used to: a remembered read
    // had to be at least as long as the write, which quietly meant "the layer
    // that served the disc handed over the whole chunk in one go". A decrypted
    // image's does - one read of everything asked for - but a Redump image's
    // does not: rpcs3 reads its first sector, then its middle, then its last, so
    // for a 64 KiB chunk of the game's the longest read remembered was 61 KiB
    // and nothing matched. The bytes are what settle it, and they are compared
    // in full below either way, so the length was never part of the judgement.
    // What it did do was keep most candidates from being read at all; the
    // scan is still the same sixty-four, and still gives up on each after
    // sixty-four bytes, but more of them now get that far. That is the price,
    // and it is paid once per file the game creates, never per write.
    bool recently_read_start(const void* buffer, u64 size, std::shared_ptr<node>& source, u64& at)
    {
      for (unsigned i = 0; i < RECENT_READS; i++)
      {
        const recent_read& r = g_recentReads[(g_recentNext + RECENT_READS - 1 - i) % RECENT_READS];
        if (r.size == 0)
          continue;
        auto src = r.source.lock();
        // the first few bytes first: most writes are not copies of anything
        if (!src || !source_holds(src, r.offset, buffer, std::min<u64>(size, 64)) || !source_holds(src, r.offset, buffer, size))
          continue;
        source = std::move(src);
        at = r.offset;
        return true;
      }
      return false;
    }

    // The mirror's bytes become its own: the source's prefix is copied in.
    void materialise(node& n)
    {
      if (!n.mirror)
        return;
      g_mirrorStats.mirrors--;
      g_mirrorStats.heldBytes -= n.mirrorLen;
      g_mirrorStats.copiedBytes += n.mirrorLen;
      n.data.resize(static_cast<size_t>(n.mirrorLen));
      auto& r = reader_for(n.mirror);
      u64 at = 0;
      while (at < n.mirrorLen)
      {
        const u64 got = r.read_at(n.mirrorAt + at, n.data.data() + at, n.mirrorLen - at);
        if (got == 0)
          break; // a source that came up short: what is missing reads as zero, which is what an unmirrored short read would have left
        at += got;
      }
      n.mirror.reset();
      n.mirrorLen = 0;
      n.mirrorAt = 0;
    }

    struct mem_file final : fs::file_base
    {
      std::shared_ptr<node> n;
      u64 pos = 0;
      bool writable;
      bool append;
      FILE* host = nullptr;

      std::unique_ptr<zip_stream> zip;
      std::unique_ptr<sz_stream> sevenz;
      // a mirror is read through a reader of THIS handle's own, so two handles
      // reading two places do not drag one compressed stream back and forth
      std::unique_ptr<source_reader> mirrorReader;
      const node* mirrorReaderOf = nullptr;

      mem_file(std::shared_ptr<node> n_, bool w, bool a) : n(std::move(n_)), writable(w), append(a)
      {
        if (!n->host_path.empty())
          host = std::fopen(n->host_path.c_str(), "rb");
        else if (n->arch)
          zip = std::make_unique<zip_stream>(n->arch, n->arch_entry);
        else if (n->sz)
          sevenz = std::make_unique<sz_stream>(n->sz, n->sz_entry);
      }
      ~mem_file() override
      {
        if (host)
          std::fclose(host);
      }
      fs::stat_t get_stat() override
      {
        fs::stat_t st{};
        fill_stat(*n, st);
        return st;
      }
      bool trunc(u64 length) override
      {
        if (!writable || host || zip || sevenz)
          return false;
        materialise(*n);
        n->data.resize(length);
        n->mtime = now_mtime();
        return true;
      }
      u64 read_at(u64 offset, void* buffer, u64 size) override
      {
        if (zip || sevenz || host)
        {
          const u64 got = read_source_at(offset, buffer, size);
          remember_read(n, offset, got);
          return got;
        }
        if (n->mirror)
        {
          if (offset >= n->mirrorLen)
            return 0;
          if (!mirrorReader || mirrorReaderOf != n->mirror.get())
          {
            mirrorReader = std::make_unique<source_reader>(*n->mirror);
            mirrorReaderOf = n->mirror.get();
          }
          return mirrorReader->read_at(n->mirrorAt + offset, buffer, std::min<u64>(size, n->mirrorLen - offset));
        }
        if (offset >= n->data.size())
          return 0;
        const u64 got = std::min<u64>(size, n->data.size() - offset);
        std::memcpy(buffer, n->data.data() + offset, got);
        return got;
      }
      u64 read_source_at(u64 offset, void* buffer, u64 size)
      {
        if (zip)
          return zip->read_at(offset, buffer, size);
        if (sevenz)
          return sevenz->read_at(offset, buffer, size);
        return host_read_at(host, n->host_path, n->host_size, offset, buffer, size);
      }
      u64 read(void* buffer, u64 size) override
      {
        const u64 got = read_at(pos, buffer, size);
        pos += got;
        return got;
      }
      u64 write(const void* buffer, u64 size) override
      {
        if (!writable || host || zip || sevenz)
          return 0;
        if (append)
          pos = logical_size(*n);
        if (size != 0)
        {
          // a write at the very end of a mirror, of the bytes the source holds at
          // the matching offset: the mirror grows. A big write into a fresh empty
          // file, of the bytes a recent read began with: a mirror starts.
          const bool atEnd = pos == logical_size(*n);
          if (atEnd && n->mirror && source_holds(n->mirror, n->mirrorAt + pos, buffer, size))
          {
            n->mirrorLen = pos + size;
            g_mirrorStats.heldBytes += size;
            pos += size;
            n->mtime = now_mtime();
            return size;
          }
          if (atEnd && !n->mirror && n->data.empty() && size >= MIRROR_MIN_START)
          {
            std::shared_ptr<node> src;
            u64 at = 0;
            if (recently_read_start(buffer, size, src, at))
            {
              n->mirror = std::move(src);
              n->mirrorAt = at;
              n->mirrorLen = size;
              g_mirrorStats.mirrors++;
              g_mirrorStats.heldBytes += size;
              pos = size;
              n->mtime = now_mtime();
              return size;
            }
          }
          // a big write onto the end of a file that no mirror could hold: this
          // one is paid for in full, and a game whose whole install reads like
          // this is the problem these figures are here to show
          if (atEnd && size >= MIRROR_MIN_START)
            g_mirrorStats.copiedBytes += size;
          materialise(*n);
        }
        if (pos + size > n->data.size())
          n->data.resize(pos + size);
        std::memcpy(n->data.data() + pos, buffer, size);
        pos += size;
        n->mtime = now_mtime();
        return size;
      }
      u64 seek(s64 offset, fs::seek_mode whence) override
      {
        const s64 base = whence == fs::seek_set ? 0 : whence == fs::seek_cur ? static_cast<s64>(pos) : static_cast<s64>(this->size());
        if (base + offset < 0)
          return -1;
        pos = static_cast<u64>(base + offset);
        return pos;
      }
      u64 size() override
      {
        return (host || zip || sevenz) ? n->host_size : logical_size(*n);
      }
    };

    struct mem_dir final : fs::dir_base
    {
      std::vector<fs::dir_entry> entries;
      size_t at = 0;
      explicit mem_dir(const node& d)
      {
        // "." and ".." first, the way every real filesystem's readdir answers
        // and the way the console's own cellFsReaddir does. They are not a
        // courtesy: sys_fs_opendir builds the directory it hands the game and
        // then sorts it from data.begin() + 2, to leave those two entries
        // where the console puts them. A directory that reports fewer than two
        // entries makes that iterator run past the end of the vector, and the
        // sort then walks memory that is not the directory - a crash inside
        // memcmp on one machine and a hang on another, because it is undefined
        // either way. An EMPTY directory is what meets it: Mortal Kombat
        // Komplete Edition creates /dev_hdd0/game/BLUS30902/USRDIR/MK9/DYNADS/
        // TEMP and opens it in the next breath. Every other reader of this
        // filesystem already skips these two by name, because every host
        // hands them back.
        for (const char* dots : { ".", ".." })
        {
          fs::dir_entry e;
          e.name = dots;
          fill_stat(d, e);
          entries.push_back(std::move(e));
        }
        for (auto& [name, child] : d.children)
        {
          fs::dir_entry e;
          e.name = name;
          fill_stat(*child, e);
          entries.push_back(std::move(e));
        }
      }
      bool read(fs::dir_entry& out) override
      {
        if (at >= entries.size())
          return false;
        out = entries[at++];
        return true;
      }
      void rewind() override
      {
        at = 0;
      }
    };

    struct memfs_device final : fs::device_base
    {
      memfs_device()
      {
        fs_prefix = memfs_root;
      }
      bool stat(const std::string& path, fs::stat_t& info) override
      {
        auto n = lookup(strip_root(path));
        if (!n)
        {
          fs::g_tls_error = fs::error::noent;
          return false;
        }
        fill_stat(*n, info);
        return true;
      }
      bool statfs(const std::string&, fs::device_stat& info) override
      {
        info.block_size = 4096;
        info.total_size = 8ull << 30;
        info.total_free = 4ull << 30;
        info.avail_free = 4ull << 30;
        return true;
      }
      bool remove_dir(const std::string& path) override
      {
        std::shared_ptr<node> parent;
        std::string leaf;
        auto n = lookup(strip_root(path), &parent, &leaf);
        if (!n || !n->dir || !parent)
        {
          fs::g_tls_error = fs::error::noent;
          return false;
        }
        if (!n->children.empty())
        {
          fs::g_tls_error = fs::error::notempty;
          return false;
        }
        parent->children.erase(leaf);
        return true;
      }
      bool create_dir(const std::string& path) override
      {
        std::shared_ptr<node> parent;
        std::string leaf;
        if (lookup(strip_root(path), &parent, &leaf))
        {
          fs::g_tls_error = fs::error::exist;
          return false;
        }
        if (!parent)
        {
          fs::g_tls_error = fs::error::noent;
          return false;
        }
        auto d = std::make_shared<node>();
        d->dir = true;
        d->mtime = now_mtime();
        parent->children[leaf] = d;
        return true;
      }
      bool rename(const std::string& from, const std::string& to) override
      {
        std::shared_ptr<node> from_parent, to_parent;
        std::string from_leaf, to_leaf;
        auto n = lookup(strip_root(from), &from_parent, &from_leaf);
        if (!n || !from_parent)
        {
          fs::g_tls_error = fs::error::noent;
          return false;
        }
        auto existing = lookup(strip_root(to), &to_parent, &to_leaf);
        if (!to_parent)
        {
          fs::g_tls_error = fs::error::noent;
          return false;
        }
        if (existing && existing->dir && !existing->children.empty())
        {
          fs::g_tls_error = fs::error::notempty;
          return false;
        }
        from_parent->children.erase(from_leaf);
        to_parent->children[to_leaf] = n;
        n->mtime = now_mtime();
        return true;
      }
      bool remove(const std::string& path) override
      {
        std::shared_ptr<node> parent;
        std::string leaf;
        auto n = lookup(strip_root(path), &parent, &leaf);
        if (!n || n->dir || !parent)
        {
          fs::g_tls_error = fs::error::noent;
          return false;
        }
        parent->children.erase(leaf);
        return true;
      }
      bool trunc(const std::string& path, u64 length) override
      {
        auto n = lookup(strip_root(path));
        if (!n || n->dir || !n->host_path.empty() || n->arch || n->sz)
        {
          fs::g_tls_error = fs::error::noent;
          return false;
        }
        materialise(*n);
        n->data.resize(length);
        n->mtime = now_mtime();
        return true;
      }
      bool utime(const std::string& path, s64, s64 mtime) override
      {
        auto n = lookup(strip_root(path));
        if (!n)
          return false;
        n->mtime = mtime;
        return true;
      }
      std::unique_ptr<fs::file_base> open(const std::string& path, bs_t<fs::open_mode> mode) override
      {
        std::shared_ptr<node> parent;
        std::string leaf;
        auto n = lookup(strip_root(path), &parent, &leaf);
        const bool want_write = !!(mode & fs::write) || !!(mode & fs::append);
        if (!n)
        {
          if (!(mode & fs::create) || !parent)
          {
            fs::g_tls_error = fs::error::noent;
            return nullptr;
          }
          n = std::make_shared<node>();
          n->mtime = now_mtime();
          parent->children[leaf] = n;
        }
        else if (mode & fs::excl)
        {
          fs::g_tls_error = fs::error::exist;
          return nullptr;
        }
        if (n->dir)
        {
          fs::g_tls_error = fs::error::isdir;
          return nullptr;
        }
        if (want_write && (!n->host_path.empty() || n->arch || n->sz))
        {
          fs::g_tls_error = fs::error::readonly;
          return nullptr;
        }
        if ((mode & fs::trunc) && want_write)
        {
          n->mirror.reset();
          n->mirrorLen = 0;
          n->mirrorAt = 0;
          n->data.clear();
          n->mtime = now_mtime();
        }
        return std::make_unique<mem_file>(n, want_write, !!(mode & fs::append));
      }
      std::unique_ptr<fs::dir_base> open_dir(const std::string& path) override
      {
        auto n = lookup(strip_root(path));
        if (!n || !n->dir)
        {
          fs::g_tls_error = fs::error::noent;
          return nullptr;
        }
        return std::make_unique<mem_dir>(*n);
      }
    };

    bool g_installed = false;
  }  // namespace

  void memfs_install()
  {
    if (g_installed)
      return;
    g_root = std::make_shared<node>();
    g_root->dir = true;
    fs::set_virtual_device(memfs_device_name, stx::make_shared<memfs_device>());
    g_installed = true;
  }

  void memfs_mkdirs(const std::string& rel)
  {
    mkdirs(rel);
  }

  bool memfs_graft(const std::string& rel, const std::string& host_path)
  {
    FILE* f = std::fopen(host_path.c_str(), "rb");
    if (!f)
      return false;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fclose(f);
    auto parts = split(rel);
    if (parts.empty())
      return false;
    std::string dir;
    for (size_t i = 0; i + 1 < parts.size(); i++)
      dir += parts[i] + "/";
    auto d = mkdirs(dir);
    if (!d)
      return false;
    auto n = std::make_shared<node>();
    n->host_path = host_path;
    n->host_size = size < 0 ? 0 : static_cast<u64>(size);
    n->mtime = now_mtime();
    d->children[parts.back()] = n;
    return true;
  }

  void memfs_graft_sz_entry(const std::string& rel, std::shared_ptr<const sz_index> index, size_t entry, unsigned long long size)
  {
    auto parts = split(rel);
    if (parts.empty())
      return;
    std::string dir;
    for (size_t i = 0; i + 1 < parts.size(); i++)
      dir += parts[i] + "/";
    auto d = mkdirs(dir);
    if (!d)
      return;
    auto n = std::make_shared<node>();
    n->sz = std::move(index);
    n->sz_entry = entry;
    n->host_size = size;
    n->mtime = now_mtime();
    d->children[parts.back()] = n;
  }

  void memfs_graft_zip_entry(const std::string& rel, std::shared_ptr<const zip_index> index, size_t entry, unsigned long long size)
  {
    auto parts = split(rel);
    if (parts.empty())
      return;
    std::string dir;
    for (size_t i = 0; i + 1 < parts.size(); i++)
      dir += parts[i] + "/";
    auto d = mkdirs(dir);
    if (!d)
      return;
    auto n = std::make_shared<node>();
    n->arch = std::move(index);
    n->arch_entry = entry;
    n->host_size = size;
    n->mtime = now_mtime();
    d->children[parts.back()] = n;
  }

  void memfs_mark_disc_image(const std::string& rel)
  {
    auto n = lookup(rel);
    if (!n || n->dir || n->host_path.empty())
      return;
    if (n->disc_dec_tried)
      return;
    n->disc_dec_tried = true;
    // Worked out HERE, with the disc grafted and the machine not yet running:
    // it reads the image's region table and probes for the key beside it, both
    // of which are reads of this filesystem, and doing that lazily would mean
    // doing it in the middle of deciding whether a write is a copy - reads of
    // nobody's, landing in the ring of recent reads that decision consults.
    auto dec = std::make_shared<iso_file_decryption>();
    if (!dec->init(memfs_root + "/" + rel))
      return;
    // A plain image reads the same either way: leave it on the cheap path.
    if (dec->get_enc_type() == iso_encryption_type::NONE)
      return;
    n->disc_dec = std::move(dec);
  }

  bool memfs_disc_image_is_encrypted(const std::string& rel)
  {
    auto n = lookup(rel);
    return n && (n->disc_dec || n->disc_dec_off);
  }

  void memfs_set_disc_decryption(const std::string& rel, bool on)
  {
    auto n = lookup(rel);
    if (!n)
      return;
    if (on && n->disc_dec_off)
      n->disc_dec = std::move(n->disc_dec_off);
    else if (!on && n->disc_dec)
      n->disc_dec_off = std::move(n->disc_dec);
    // the readers cache nothing of the decision, but they do cache an open
    // handle and a scratch buffer: start them again so neither is half of one
    // answer and half of the other
    g_sourceReaders.clear();
  }

  memfs_mirror_stats memfs_mirror_report()
  {
    return g_mirrorStats;
  }

  void memfs_mirror_reset()
  {
    g_mirrorStats = memfs_mirror_stats{};
  }

  void memfs_put(const std::string& rel, const void* data, size_t size)
  {
    auto parts = split(rel);
    std::string dir;
    for (size_t i = 0; i + 1 < parts.size(); i++)
      dir += parts[i] + "/";
    auto d = mkdirs(dir);
    auto n = std::make_shared<node>();
    n->data.assign(static_cast<const u8*>(data), static_cast<const u8*>(data) + size);
    n->mtime = now_mtime();
    d->children[parts.back()] = n;
  }

  void memfs_list(const std::string& rel_dir, std::vector<memfs_file>& out)
  {
    auto start = lookup(rel_dir);
    if (!start || !start->dir)
      return;
    // depth first in name order (std::map): the same list every time, which is
    // what makes an export comparable with another
    struct frame { std::shared_ptr<node> n; std::string prefix; };
    std::vector<frame> stack{{start, ""}};
    while (!stack.empty())
    {
      auto [n, prefix] = stack.back();
      stack.pop_back();
      for (auto it = n->children.rbegin(); it != n->children.rend(); ++it)
      {
        const auto& [name, child] = *it;
        if (child->dir)
          stack.push_back({child, prefix + name + "/"});
      }
      for (auto& [name, child] : n->children)
      {
        // a grafted file is the host's, read-only: it is not something the
        // machine saved
        if (child->dir || readOnly(*child))
          continue;
        // a mirror of a disc file is the game's own installation, not something it
        // saved; and it has no bytes of its own to point at
        if (child->mirror)
          continue;
        out.push_back({prefix + name, child->data.data(), child->data.size()});
      }
    }
  }

  size_t memfs_bytes()
  {
    size_t total = 0;
    std::vector<std::shared_ptr<node>> stack{g_root};
    while (!stack.empty())
    {
      auto n = stack.back();
      stack.pop_back();
      total += n->data.size();
      for (auto& [k, c] : n->children)
        stack.push_back(c);
    }
    return total;
  }
}  // namespace chimera
