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
#include <vector>

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

  // Say that a grafted file is a PS3 disc IMAGE. The bytes such an image holds
  // are not always the bytes the console reads from it: a Redump dump keeps the
  // disc's data regions encrypted and rpcs3's ISO layer decrypts them on the
  // way past. A file the game copies off the disc therefore matches the image
  // only once the image is read the way the console reads it, which is what
  // this enables - see "not carrying the disc twice" in memfs.cpp.
  void memfs_mark_disc_image(const std::string& rel);

  // Whether that image turned out to need decrypting to be read (for the log).
  bool memfs_disc_image_is_encrypted(const std::string& rel);

  // Read that image AS IT LIES instead, or through its key again. The gate's
  // negative control: with this off, a file copied off an encrypted disc
  // matches nothing and is kept in full, which is the state the fix is for.
  // Nothing in a normal run touches it.
  void memfs_set_disc_decryption(const std::string& rel, bool on);

  // Put bytes in a file (created, truncated).
  void memfs_put(const std::string& rel, const void* data, size_t size);

  // Every in-memory file under a directory (path relative to the root), in name
  // order, with a pointer to its bytes. For the save-data export: the pointers
  // are the files themselves and hold only until the machine runs again.
  struct memfs_file
  {
    std::string rel; // relative to the directory asked for, '/' separated
    const unsigned char* data;
    size_t size;
  };
  void memfs_list(const std::string& rel_dir, std::vector<memfs_file>& out);

  // Diagnostics: total bytes held in memory files.
  size_t memfs_bytes();

  // Diagnostics: how the disc mirror fared, which is the difference between a
  // game whose own data install costs a savestate nothing and one whose install
  // costs it several gigabytes. `heldBytes` is what the mirrors stand for and
  // are not paying for; `copiedBytes` counts the writes big enough to have
  // started a mirror that matched no disc file and so were kept in full.
  struct memfs_mirror_stats
  {
    unsigned long long mirrors = 0;      // files held as a reference to a source
    unsigned long long heldBytes = 0;    // what those files would have weighed
    unsigned long long copiedBytes = 0;  // big writes no mirror could hold
    unsigned long long decryptedBytes = 0;  // read through a disc's decryption to compare
  };
  memfs_mirror_stats memfs_mirror_report();

  // Start the figures again. Said once the machine is built and before it is
  // sealed, so that what they report is what the MACHINE has done since - which
  // is what a savestate carries. What the emulator installed before that (the
  // firmware, a package) is in the baseline and costs a state nothing.
  void memfs_mirror_reset();
}
