// See memfs.h.
// SPDX-License-Identifier: MIT
#include "stdafx.h"

#include "Utilities/File.h"
#include "util/shared_ptr.hpp"

#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "memfs.h"

#include "archive.h"

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
      std::shared_ptr<const zip_index> arch;             // zip entry (read-only)
      size_t arch_entry = 0;
      std::map<std::string, std::shared_ptr<node>> children;
      s64 mtime = 0;
    };

    std::shared_ptr<node> g_root;
    u64 g_clock = 1;  // a monotonically increasing "mtime", never the wall clock

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
          child->mtime = static_cast<s64>(g_clock++);
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
      st.is_writable = n.host_path.empty() && !n.arch;
      st.size = n.dir ? 0 : ((n.host_path.empty() && !n.arch) ? n.data.size() : n.host_size);
      st.atime = st.mtime = st.ctime = n.mtime;
    }

    struct mem_file final : fs::file_base
    {
      std::shared_ptr<node> n;
      u64 pos = 0;
      bool writable;
      bool append;
      FILE* host = nullptr;

      std::unique_ptr<zip_stream> zip;

      mem_file(std::shared_ptr<node> n_, bool w, bool a) : n(std::move(n_)), writable(w), append(a)
      {
        if (!n->host_path.empty())
          host = std::fopen(n->host_path.c_str(), "rb");
        else if (n->arch)
          zip = std::make_unique<zip_stream>(n->arch, n->arch_entry);
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
        if (!writable || host || zip)
          return false;
        n->data.resize(length);
        n->mtime = static_cast<s64>(g_clock++);
        return true;
      }
      u64 read_at(u64 offset, void* buffer, u64 size) override
      {
        if (zip)
          return zip->read_at(offset, buffer, size);
        if (host)
        {
          if (offset >= n->host_size)
            return 0;
          if (std::fseek(host, static_cast<long>(offset), SEEK_SET) != 0)
            return 0;
          return std::fread(buffer, 1, static_cast<size_t>(std::min<u64>(size, n->host_size - offset)), host);
        }
        if (offset >= n->data.size())
          return 0;
        const u64 got = std::min<u64>(size, n->data.size() - offset);
        std::memcpy(buffer, n->data.data() + offset, got);
        return got;
      }
      u64 read(void* buffer, u64 size) override
      {
        const u64 got = read_at(pos, buffer, size);
        pos += got;
        return got;
      }
      u64 write(const void* buffer, u64 size) override
      {
        if (!writable || host || zip)
          return 0;
        if (append)
          pos = n->data.size();
        if (pos + size > n->data.size())
          n->data.resize(pos + size);
        std::memcpy(n->data.data() + pos, buffer, size);
        pos += size;
        n->mtime = static_cast<s64>(g_clock++);
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
        return (host || zip) ? n->host_size : n->data.size();
      }
    };

    struct mem_dir final : fs::dir_base
    {
      std::vector<fs::dir_entry> entries;
      size_t at = 0;
      explicit mem_dir(const node& d)
      {
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
        d->mtime = static_cast<s64>(g_clock++);
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
        n->mtime = static_cast<s64>(g_clock++);
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
        if (!n || n->dir || !n->host_path.empty() || n->arch)
        {
          fs::g_tls_error = fs::error::noent;
          return false;
        }
        n->data.resize(length);
        n->mtime = static_cast<s64>(g_clock++);
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
          n->mtime = static_cast<s64>(g_clock++);
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
        if (want_write && (!n->host_path.empty() || n->arch))
        {
          fs::g_tls_error = fs::error::readonly;
          return nullptr;
        }
        if ((mode & fs::trunc) && want_write)
        {
          n->data.clear();
          n->mtime = static_cast<s64>(g_clock++);
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
    n->mtime = static_cast<s64>(g_clock++);
    d->children[parts.back()] = n;
    return true;
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
    n->mtime = static_cast<s64>(g_clock++);
    d->children[parts.back()] = n;
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
    n->mtime = static_cast<s64>(g_clock++);
    d->children[parts.back()] = n;
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
