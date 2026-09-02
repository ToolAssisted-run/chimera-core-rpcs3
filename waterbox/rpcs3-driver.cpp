// The adapter between the RPCS3 emulator library and the Chimera core
// surface: fills the emulator's callback table with headless answers, pins
// the configuration to the deterministic machine, boots, and drives one
// vblank period of virtual time per frame through vsched.
// SPDX-License-Identifier: MIT

#include "stdafx.h"

#include "Emu/System.h"
#include "Emu/system_config.h"
#include "Emu/vfs_config.h"
#include "Emu/IdManager.h"
#include "Emu/VFS.h"
#include "Emu/Memory/vm.h"
#include "Emu/Cell/PPUThread.h"
#include "Emu/RSX/Null/NullGSRender.h"
#include "Emu/RSX/GSFrameBase.h"
#include "Emu/Audio/AudioBackend.h"
#include "Emu/Audio/Null/NullAudioBackend.h"
#include "Emu/Audio/Null/null_enumerator.h"
#include "Emu/Io/Null/NullKeyboardHandler.h"
#include "Emu/Io/Null/NullMouseHandler.h"
#include "Emu/Io/Null/null_camera_handler.h"
#include "Emu/Io/Null/null_music_handler.h"
#include "Emu/Io/KeyboardHandler.h"
#include "Emu/Io/MouseHandler.h"
#include "Emu/Cell/Modules/cellMsgDialog.h"
#include "Emu/Cell/Modules/cellOskDialog.h"
#include "Emu/Cell/Modules/cellSaveData.h"
#include "Emu/Cell/Modules/sceNpTrophy.h"
#include "Input/pad_thread.h"
#include "Utilities/File.h"
#include "Utilities/Thread.h"
#include "util/video_source.h"
#include "util/logs.hpp"

#include <deque>
#include <string>

#include "rpcs3-driver.h"
#include "vsched.h"

// the directory roots the emulator library reads (patch 0003 routes
// fs::get_config_dir/get_cache_dir to these, the Android way)
extern std::string g_android_executable_dir;
extern std::string g_android_config_dir;
extern std::string g_android_cache_dir;
extern fs::file g_tty;

LOG_CHANNEL(chimera_log, "CHIMERA");

namespace
{
  std::string g_error;
  std::string g_work;
  std::string g_tty_path;
  std::vector<u8> g_tty_bytes;
  u64 g_tty_read = 0;
  u64 g_frame_index = 0;
  u64 g_frame_base_ns = 0;
  bool g_booted = false;

  // "call from main thread": the driver thread is vsched thread 0 and runs
  // this queue whenever it holds the machine.
  struct main_task
  {
    std::function<void()> func;
    atomic_t<u32>* wake_up;
  };
  std::deque<main_task> g_main_queue;
  atomic_t<u32> g_main_word{0};

  void run_main_queue()
  {
    while (!g_main_queue.empty())
    {
      main_task t = std::move(g_main_queue.front());
      g_main_queue.pop_front();
      t.func();
      if (t.wake_up)
      {
        *t.wake_up = 1;
        t.wake_up->notify_one();
      }
    }
  }

  void fail(const std::string& what)
  {
    g_error = what;
    chimera_log.error("%s", what);
  }

  // The window the RSX backend presents to: there is none. The null backend
  // never draws, and the bridged GL backend arrives with its milestone.
  class headless_frame final : public GSFrameBase
  {
  public:
    void close() override {}
    void reset() override {}
    bool shown() override { return true; }
    void hide() override {}
    void show() override {}
    void toggle_fullscreen() override {}
    void delete_context(draw_context_t) override {}
    draw_context_t make_context() override { return nullptr; }
    void set_current(draw_context_t) override {}
    void flip(draw_context_t, bool) override {}
    int client_width() override { return 1280; }
    int client_height() override { return 720; }
    f64 client_display_rate() override { return 60.0; }
    bool has_alpha() override { return false; }
    // the handle type is a variant of window-system handles and headless has
    // none: the null backend never asks
    display_handle_t handle() const override { fmt::throw_exception("chimera: no display handle"); }
    bool can_consume_frame() const override { return false; }
    void present_frame(std::vector<u8>&&, u32, u32, u32, bool) const override {}
    void take_screenshot(std::vector<u8>&&, u32, u32, bool) override {}
    void update_title(double) override {}
  };

  EmuCallbacks make_callbacks()
  {
    EmuCallbacks cb{};

    cb.call_from_main_thread = [](std::function<void()> func, atomic_t<u32>* wake_up)
    {
      if (vsched_current_id() == 0)
      {
        // already on the driver thread: run it now, in order with whatever
        // the driver was doing
        func();
        if (wake_up)
        {
          *wake_up = 1;
          wake_up->notify_one();
        }
        return;
      }
      g_main_queue.push_back({std::move(func), wake_up});
      if (getenv("CHIMERA_TRACE"))
        fprintf(stderr, "[chimera] main-thread task queued from thread %u (queue %zu)\n", vsched_current_id(), g_main_queue.size());
      g_main_word++;
      g_main_word.notify_one();
    };
    cb.try_to_quit = [](bool, std::function<void()>) { return false; };

    cb.init_gs_render = [](utils::serial* ar)
    {
      switch (g_cfg.video.renderer.get())
      {
      case video_renderer::null:
        g_fxo->init<rsx::thread, named_thread<NullGSRender>>(ar);
        break;
      default:
        fmt::throw_exception("chimera: renderer %s is not wired yet", g_cfg.video.renderer.get());
      }
    };
    cb.get_gs_frame = []() -> std::unique_ptr<GSFrameBase> { return std::make_unique<headless_frame>(); };
    cb.close_gs_frame = []() {};

    cb.init_kb_handler = []() { ensure(g_fxo->init<KeyboardHandlerBase, NullKeyboardHandler>(Emu.DeserialManager())); };
    cb.init_mouse_handler = []() { ensure(g_fxo->init<MouseHandlerBase, NullMouseHandler>(Emu.DeserialManager())); };
    cb.init_pad_handler = [](std::string_view title_id)
    {
      ensure(g_fxo->init<named_thread<pad_thread>>(nullptr, nullptr, title_id));
      // the pad thread announces itself once it has bound its handlers
      while (!pad::g_started)
      {
        vsched_yield_default();
      }
    };

    cb.get_audio = []() -> std::shared_ptr<AudioBackend> { return std::make_shared<NullAudioBackend>(); };
    cb.get_audio_enumerator = [](u64) -> std::shared_ptr<audio_device_enumerator> { return std::make_shared<null_enumerator>(); };
    cb.get_camera_handler = []() -> std::shared_ptr<camera_handler_base> { return std::make_shared<null_camera_handler>(); };
    cb.get_music_handler = []() -> std::shared_ptr<music_handler_base> { return std::make_shared<null_music_handler>(); };

    cb.get_msg_dialog = []() -> std::shared_ptr<MsgDialogBase> { return {}; };
    cb.get_osk_dialog = []() -> std::shared_ptr<OskDialogBase> { return {}; };
    cb.get_save_dialog = []() -> std::unique_ptr<SaveDialogBase> { return {}; };
    cb.get_sendmessage_dialog = []() -> std::shared_ptr<SendMessageDialogBase> { return {}; };
    cb.get_recvmessage_dialog = []() -> std::shared_ptr<RecvMessageDialogBase> { return {}; };
    cb.get_trophy_notification_dialog = []() -> std::unique_ptr<TrophyNotificationBase> { return {}; };

    cb.on_run = [](bool) {};
    cb.on_pause = []() {};
    cb.on_resume = []() {};
    cb.on_stop = []() {};
    cb.on_ready = []() {};
    cb.on_missing_fw = []() { fail("the PS3 firmware is missing and the game needs it"); };
    cb.on_emulation_stop_no_response = [](std::shared_ptr<atomic_t<bool>> closed, int seconds)
    {
      if (!closed || !*closed)
      {
        chimera_log.error("stopping the emulator took %d seconds of machine time", seconds);
      }
    };
    cb.on_save_state_progress = [](std::shared_ptr<atomic_t<bool>>, stx::shared_ptr<utils::serial>, stx::atomic_ptr<std::string>*, std::shared_ptr<void>) {};
    cb.enable_disc_eject = [](bool) {};
    cb.enable_disc_insert = [](bool) {};
    cb.handle_taskbar_progress = [](s32, s32) {};
    cb.update_emu_settings = []() {};
    cb.save_emu_settings = []() {};

    cb.get_localized_string = [](localized_string_id, const char*) -> std::string { return {}; };
    cb.get_localized_u32string = [](localized_string_id, const char*) -> std::u32string { return {}; };
    cb.get_localized_setting = [](const cfg::_base*, u32) -> std::string { return {}; };
    cb.get_photo_path = [](std::string_view) -> std::string { return {}; };
    cb.play_sound = [](const std::string&, std::optional<f32>) {};
    cb.get_image_info = [](const std::string&, std::string&, s32&, s32&, s32&) { return false; };
    cb.get_scaled_image = [](const std::string&, s32, s32, s32&, s32&, u8*, bool) { return false; };
    cb.get_font_dirs = []() -> std::vector<std::string> { return {}; };
    cb.on_install_pkgs = [](const std::vector<std::string>&, bool) { return false; };
    cb.add_breakpoint = [](u32) {};
    cb.display_sleep_control_supported = []() { return false; };
    cb.enable_display_sleep = [](bool) {};
    cb.check_microphone_permissions = []() {};
    cb.make_video_source = []() -> std::unique_ptr<video_source> { return nullptr; };
    cb.enable_gamemode = [](bool) {};
    cb.get_database_config = [](const std::string&) -> std::string { return {}; };
    return cb;
  }

  // The machine: every knob that would make two runs differ, pinned.
  void pin_configuration()
  {
    g_cfg.core.ppu_decoder.set(ppu_decoder_type::_static);
    g_cfg.core.spu_decoder.set(spu_decoder_type::_static);
    g_cfg.core.spu_cache.set(false);
    g_cfg.core.llvm_precompilation.set(false);
    g_cfg.core.spu_loop_detection.set(false);
    g_cfg.core.use_accurate_dfma.set(true);
    g_cfg.core.ppu_threads.set(2);
    g_cfg.core.clocks_scale.set(100);
    g_cfg.core.max_cpu_preempt_count_per_frame.set(0);
    g_cfg.core.thread_scheduler.set(thread_scheduler_mode::os);
    g_cfg.video.renderer.set(video_renderer::null);
    g_cfg.video.frame_limit.set(frame_limit_type::_auto);
    g_cfg.video.vblank_rate.set(60);
    g_cfg.video.vblank_ntsc.set(false);
    g_cfg.video.multithreaded_rsx.set(false);
    g_cfg.audio.renderer.set(audio_renderer::null);
    g_cfg.audio.enable_buffering.set(false);
    g_cfg.audio.enable_time_stretching.set(false);
    g_cfg.io.keyboard.set(keyboard_handler::null);
    g_cfg.io.mouse.set(mouse_handler::null);
    g_cfg.io.camera.set(camera_handler::null);
    g_cfg.io.move.set(move_handler::null);
    g_cfg.io.pad_mode.set(pad_handler_mode::single_threaded);
    g_cfg.io.mouse_based_gyro_enabled.set(false);
    g_cfg.sys.console_time_offset.set(0);
    g_cfg.sys.system_name.from_string("Chimera");
    g_cfg.sys.console_psid.from_string("0x0000000000000000");
    g_cfg.misc.autostart.set(false);
    g_cfg.misc.enable_gamemode.set(false);
    // no firmware yet: the two startup libraries are HLE, nothing is loaded
    // from dev_flash (M3 turns this around)
    g_cfg.core.libraries_control.set_set({"liblv2.sprx:hle", "libsysmodule.sprx:hle"});
  }

  void ensure_dir(const std::string& p)
  {
    fs::create_path(p);
  }
}  // namespace

extern "C" {

const char* chimera_rpcs3_error(void)
{
  return g_error.c_str();
}

int chimera_rpcs3_init(const char* work_dir, const char* game_path)
{
  g_error.clear();
  g_work = work_dir;
  if (!g_work.empty() && g_work.back() != '/')
    g_work += '/';
  g_android_executable_dir = g_work;
  g_android_config_dir = g_work + "config/";
  g_android_cache_dir = g_work + "cache/";
  ensure_dir(g_android_config_dir);
  ensure_dir(g_android_cache_dir);
  // the firmware gate looks for this one file on the host path; an empty
  // file satisfies it, and the startup libraries are HLE so it is never read
  ensure_dir(g_android_config_dir + "dev_flash/sys/external/");
  if (!fs::is_file(g_android_config_dir + "dev_flash/sys/external/liblv2.sprx"))
  {
    fs::file(g_android_config_dir + "dev_flash/sys/external/liblv2.sprx", fs::create + fs::write);
  }

  // the emulator logs nowhere without a listener; the work dir gets one
  static std::unique_ptr<logs::listener> s_log = logs::make_file_listener(g_work + "RPCS3.log", 64 * 1024 * 1024);

  vsched_init();

  Emu.SetHasGui(false);
  Emu.SetHeadless(true);
  Emu.SetSupportedRenderers({video_renderer::null});
  Emu.SetDefaultRenderer(video_renderer::null);
  Emu.SetCallbacks(make_callbacks());
  Emu.SetUsr("00000001");
  Emu.Init();

  pin_configuration();
  Emulator::SaveSettings(g_cfg.to_string(), "");

  g_tty_path = g_android_cache_dir + "TTY.log";

  const game_boot_result r = Emu.BootGame(game_path, "", true, cfg_mode::custom);
  run_main_queue();
  if (r != game_boot_result::no_errors)
  {
    fail(fmt::format("boot failed: %s", r));
    return 0;
  }
  if (!Emu.IsReady())
  {
    fail(fmt::format("boot did not reach the ready state (state %d)", static_cast<int>(Emu.GetStatus(false))));
    return 0;
  }
  Emu.Run(true);
  run_main_queue();
  g_booted = true;
  g_frame_base_ns = vsched_now_ns();
  g_frame_index = 0;
  return 1;
}

void chimera_rpcs3_frame(void)
{
  if (!g_booted)
    return;
  g_frame_index++;
  // the vblank thread itself computes start + n * 1000000 / 60 in whole
  // microseconds; the frame ends on that same grid
  const u64 frame_end = g_frame_base_ns + (g_frame_index * 1000000ull / 60) * 1000ull;
  while (vsched_now_ns() < frame_end)
  {
    const u32 seen = g_main_word;
    run_main_queue();
    if (vsched_now_ns() >= frame_end)
      break;
    vsched_wait_item item = {&g_main_word, seen};
    vsched_wait(&item, 1, frame_end - vsched_now_ns());
  }
  run_main_queue();
}

void chimera_rpcs3_shutdown(void)
{
  if (!g_booted)
    return;
  g_booted = false;
  Emu.Kill(false);
  // the emulator's own join thread tears the machine down and hands the
  // final step back to the driver thread as a task: pump until stopped
  const u64 deadline = vsched_now_ns() + 30ull * 1000000000ull;
  while (!Emu.IsStopped(true) && vsched_now_ns() < deadline)
  {
    run_main_queue();
    vsched_yield_default();
  }
  run_main_queue();
  if (!Emu.IsStopped(true))
    chimera_log.error("the emulator did not stop within 30 s of machine time (state %d)", static_cast<int>(Emu.GetStatus(false)));
}

void chimera_rpcs3_read_main_memory(uint32_t offset, uint32_t size, uint8_t* dst)
{
  // page by page: unmapped pages are zero
  u32 done = 0;
  while (done < size)
  {
    const u32 addr = offset + done;
    const u32 in_page = 4096 - (addr & 4095);
    const u32 n = std::min<u32>(in_page, size - done);
    if (vm::check_addr(addr, vm::page_readable, n))
      std::memcpy(dst + done, vm::g_base_addr + addr, n);
    else
      std::memset(dst + done, 0, n);
    done += n;
  }
}

uint64_t chimera_rpcs3_main_memory_digest(void)
{
  u64 h = 1469598103934665603ULL;
  for (u32 page = 0; page < 0x10000000; page += 4096)
  {
    if (!vm::check_addr(page, vm::page_readable, 4096))
      continue;
    const u8* p = vm::g_base_addr + page;
    for (u32 i = 0; i < 4096; i++)
    {
      h ^= p[i];
      h *= 1099511628211ULL;
    }
  }
  return h;
}

const uint8_t* chimera_rpcs3_tty(int64_t* size)
{
  if (g_tty)
  {
    g_tty.sync();
  }
  fs::file f(g_tty_path, fs::read);
  if (f)
  {
    const u64 total = f.size();
    if (total > g_tty_read)
    {
      f.seek(g_tty_read);
      std::vector<u8> more(total - g_tty_read);
      f.read(more.data(), more.size());
      g_tty_bytes.insert(g_tty_bytes.end(), more.begin(), more.end());
      g_tty_read = total;
    }
  }
  *size = static_cast<int64_t>(g_tty_bytes.size());
  return g_tty_bytes.data();
}

int chimera_rpcs3_vsync_numerator(void)
{
  return 60;
}

int chimera_rpcs3_vsync_denominator(void)
{
  return 1;
}

uint64_t chimera_rpcs3_machine_time_ns(void)
{
  return vsched_now_ns();
}

int chimera_rpcs3_thread_count(void)
{
  return vsched_thread_count();
}

int chimera_rpcs3_is_running(void)
{
  return Emu.IsRunning() ? 1 : 0;
}

// Debugging: where every PPU thread is, to stderr.
void chimera_rpcs3_debug_ppu(void)
{
  fprintf(stderr, "emu state=%d time=%llu threads=%d switches=%llu\n", static_cast<int>(Emu.GetStatus(false)), (unsigned long long)vsched_now_ns(), vsched_thread_count(), (unsigned long long)vsched_switch_count());
  idm::select<named_thread<ppu_thread>>([](u32 id, ppu_thread& p)
  {
    fprintf(stderr, "PPU id=%x cia=%08x lr=%llx state=%x gpr:", id, p.cia, (unsigned long long)p.lr, static_cast<u32>(p.state.load()));
    for (int i = 0; i < 32; i++)
      fprintf(stderr, " r%d=%llx", i, (unsigned long long)p.gpr[i]);
    fprintf(stderr, "\n");
  });
}

}  // extern "C"
