// An in-memory filesystem for the emulator's own use: config, cache, logs,
// dev_hdd0 and dev_flash live here, and files the host mounts (the game,
// the firmware) are grafted in read-only. The emulator reaches it through
// rpcs3's virtual-device layer, so nothing in rpcs3 knows the difference
// between this and a directory on disk. Deterministic by construction: no
// timestamps from the clock, entries enumerate in name order.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <memory>
#include <string>

namespace chimera
{
  struct zip_index;
  struct sz_index;

  // The device's root path as rpcs3 sees it. Everything under it is ours.
  // (The shape is prescribed by fs::get_virtual_device: "/vfsv0_" + 22
  // characters + "_" at index 29, and set_virtual_device is keyed by the
  // part after the first underscore past index 7.)
  inline const std::string memfs_root = "/vfsv0_chimera_core_memory_fs_dev";
  inline const std::string memfs_device_name = "core_memory_fs_dev";

  // Register the device with rpcs3's filesystem layer. Idempotent.
  void memfs_install();

  // Create a directory tree under the root (path relative to the root,
  // slash separated).
  void memfs_mkdirs(const std::string& rel);

  // Graft a host-visible file into the tree, read-only, read lazily
  // through C stdio (a native path, or a name in the sandbox's file list).
  bool memfs_graft(const std::string& rel, const std::string& host_path);

  // Graft one entry of a zip archive, read-only, decompressed on demand. The
  // archive is never unpacked: a disc dumped as a folder is tens of gigabytes
  // and the sandbox has nowhere to put that.
  void memfs_graft_zip_entry(const std::string& rel, std::shared_ptr<const zip_index> index, size_t entry, unsigned long long size);

  // The same for one entry of a .7z. Decompressed on demand as a zip entry is,
  // but what it costs to reach depends on how the archive was packed - see
  // sevenzip.h on solid blocks.
  void memfs_graft_sz_entry(const std::string& rel, std::shared_ptr<const sz_index> index, size_t entry, unsigned long long size);

  // Put bytes in a file (created, truncated).
  void memfs_put(const std::string& rel, const void* data, size_t size);

  // Diagnostics: total bytes held in memory files.
  size_t memfs_bytes();
}
