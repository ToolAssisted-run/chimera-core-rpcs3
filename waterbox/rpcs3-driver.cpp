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
#include "Emu/Cell/SPUThread.h"
#include "Emu/RSX/Null/NullGSRender.h"
#include "Emu/RSX/GSFrameBase.h"
#include "Emu/Audio/AudioBackend.h"
#include "Emu/Audio/Null/NullAudioBackend.h"
#ifdef CHIMERA_GL_BRIDGE
#include "Emu/RSX/GL/GLGSRender.h"
#endif
#include "util/video_provider.h"
#include "Emu/CPU/CPUThread.h"
#include "Emu/Memory/vm_locking.h"
#include "Emu/system_progress.hpp"
#include "Emu/VFS.h"
#include "Utilities/Thread.h"
extern atomic_t<recording_mode> g_recording_mode;
u32 g_chimera_precompile_index = 0, g_chimera_precompile_count = 0, g_chimera_precompile_done = 0, g_chimera_precompile_total = 0, g_chimera_precompile_parts_done = 0, g_chimera_precompile_parts_total = 0;
// the sweep gives each session different files; inside one, its parts are all its own
u32 g_chimera_precompile_in_sweep = 0;
void chimera_ppu_precompile(std::vector<std::string>& dir_queue);
extern void ppu_initialize();
namespace rsx { extern std::function<bool(u32 addr, bool is_writing)> g_access_violation_handler; }
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
#include "Emu/Io/PadHandler.h"
#include "Emu/Io/pad_types.h"
#include "Emu/Cell/Modules/cellPad.h"
#include "Emu/RSX/RSXThread.h"
#include "Emu/RSX/rsx_utils.h"
#include "Loader/PUP.h"
#include "Loader/TAR.h"
#include "Crypto/unself.h"
#include "Crypto/key_vault.h"
#include "util/sysinfo.hpp"
#include "Utilities/File.h"
#include "Utilities/Thread.h"
#include "util/video_source.h"
#include "util/logs.hpp"

#include <deque>
#include <string>

#include "memfs.h"
#include "rpcs3-driver.h"
#include <unistd.h>
#include "chimera-assets.h"
#include "cache-bridge.h"
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
  std::string g_firmware_version;

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

  // ---- the frame's input, output and the machine's view of them ----------
  constexpr int PORTS = 7;
  constexpr int BUTTONS = 17;  // Up Down Left Right Select Start L3 R3 Triangle Circle Cross Square L1 R1 L2 R2 PS
  constexpr int AXES = 4;      // LX LY RX RY
  bool g_port_present[PORTS] = {true, false, false, false, false, false, false};
  bool g_button[PORTS][BUTTONS];
  u8 g_axis[PORTS][AXES] = {{128, 128, 128, 128}, {128, 128, 128, 128}, {128, 128, 128, 128}, {128, 128, 128, 128}, {128, 128, 128, 128}, {128, 128, 128, 128}, {128, 128, 128, 128}};
  bool g_input_read = false;

  // the wire order above as the pad's (offset, keycode) pairs
  const std::pair<u32, u32> g_button_codes[BUTTONS] = {
      {CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_UP},       {CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_DOWN},
      {CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_LEFT},     {CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_RIGHT},
      {CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_SELECT},   {CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_START},
      {CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_L3},       {CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_R3},
      {CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_TRIANGLE}, {CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_CIRCLE},
      {CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_CROSS},    {CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_SQUARE},
      {CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_L1},       {CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_R1},
      {CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_L2},       {CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_R2},
      {CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_PS},
  };
  const u32 g_axis_offsets[AXES] = {CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X, CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y, CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X, CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y};

  // The core's pad handler: a DualShock 3 per present port whose state is
  // whatever the frame set. rpcs3's pad thread calls process() every
  // pad_sleep of machine time; cellPadGetData reads the external lists.
  class chimera_pad_handler final : public PadHandlerBase
  {
  public:
    chimera_pad_handler() : PadHandlerBase(pad_handler::null)
    {
      b_has_pressure_intensity_button = false;
    }
    void init_config(cfg_pad* cfg) override
    {
      if (cfg)
        cfg->from_default();
    }
    std::vector<pad_list_entry> list_devices() override
    {
      return {pad_list_entry("Chimera Pad", false)};
    }
    bool bindPadToDevice(std::shared_ptr<Pad> pad) override
    {
      const u32 port = pad->m_player_id;
      if (port >= PORTS || !g_port_present[port])
        return false;
      pad->m_port_status = CELL_PAD_STATUS_CONNECTED | CELL_PAD_STATUS_ASSIGN_CHANGES;
      pad->m_device_capability = CELL_PAD_CAPABILITY_PS3_CONFORMITY | CELL_PAD_CAPABILITY_PRESS_MODE | CELL_PAD_CAPABILITY_HP_ANALOG_STICK | CELL_PAD_CAPABILITY_ACTUATOR | CELL_PAD_CAPABILITY_SENSOR_MODE;
      pad->m_device_type = CELL_PAD_DEV_TYPE_STANDARD;
      pad->m_class_type = CELL_PAD_PCLASS_TYPE_STANDARD;
      pad->m_class_profile = 0;
      // the pad thread mirrors these into the external lists cellPad reads
      pad->m_buttons.clear();
      for (int b = 0; b < BUTTONS; b++)
        pad->m_buttons.emplace_back(g_button_codes[b].first, std::vector<std::set<u32>>{}, g_button_codes[b].second);
      for (int a = 0; a < AXES; a++)
        pad->m_sticks[a] = AnalogStick(g_axis_offsets[a], {}, {});
      m_pads[port] = pad;
      return true;
    }
    void process() override
    {
      connected_devices = 0;
      for (int port = 0; port < PORTS; port++)
      {
        auto& pad = m_pads[port];
        if (!pad)
          continue;
        connected_devices++;
        for (int b = 0; b < BUTTONS; b++)
        {
          pad->m_buttons[b].m_pressed = g_button[port][b];
          pad->m_buttons[b].m_value = g_button[port][b] ? 255 : 0;
        }
        for (int a = 0; a < AXES; a++)
          pad->m_sticks[a].m_value = g_axis[port][a];
      }
    }

  private:
    std::shared_ptr<Pad> m_pads[PORTS];
  };

  // ---- audio: mixed blocks by machine time -------------------------------
  std::vector<s16> g_audio_ring;  // interleaved stereo
  std::vector<s16> g_audio_frame;
  u64 g_audio_residue = 0;

  // ---- video: the display buffer at flip ----------------------------------
  std::vector<u32> g_video;
  int g_video_w = 0, g_video_h = 0;

  // the renderer: what the project asked for, and whether a GPU answers
  bool s_renderer_opengl = false;
  bool s_gpu = false;
  char s_spu_decoder[16] = "asmjit";
  char s_ppu_decoder[16] = "interpreter";
  // a precompile session: boot, compile every Nth module of the sweep, stop
  int s_precompile_index = 0, s_precompile_count = 0;
  bool s_precompile_firmware = true;
  std::atomic<bool> s_precompile_done{false};
  // one of the emulator's own threads: those go through the scheduler
  std::unique_ptr<named_thread<std::function<void()>>> s_precompile_thread;
  extern "C" int chimera_rpcs3_gpu_bridge_present(void) __attribute__((weak));
  // the native reference's renderer calls the driver directly and must bind
  // the host context on its own thread; the guest resolves this to null (the
  // host dispatcher binds for it)
  extern "C" int chimera_gl_host_bind_current(void) __attribute__((weak));

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
    void set_current(draw_context_t) override
    {
      if (s_gpu && chimera_gl_host_bind_current)
        chimera_gl_host_bind_current();
    }
    // A flip: the machine's current display buffer (in guest VRAM) becomes
    // the picture. With the null backend that buffer holds whatever the
    // program wrote; the bridged GL backend fills it later.
    void flip(draw_context_t, bool) override
    {
      // the GL renderer hands its picture over through present_frame just
      // before this; nothing to fetch from VRAM
      if (s_gpu)
        return;
      auto* r = rsx::get_current_renderer();
      if (!r)
        return;
      const auto& db = r->display_buffers[r->current_display_buffer];
      if (!db.valid())
        return;
      const u32 w = db.width, h = db.height, pitch = db.pitch ? +db.pitch : w * 4;
      if (w > 4096 || h > 4096)
        return;
      const u32 addr = rsx::get_address(db.offset, CELL_GCM_LOCATION_LOCAL);
      g_video.assign(static_cast<size_t>(w) * h, 0);
      for (u32 y = 0; y < h; y++)
      {
        const u32 row = addr + y * pitch;
        if (!vm::check_addr(row, vm::page_readable, w * 4))
          continue;
        const be_t<u32>* src = vm::_ptr<be_t<u32>>(row);
        for (u32 x = 0; x < w; x++)
          // X8R8G8B8 big-endian word == BGRA little-endian; the X byte is
          // whatever the program left there, and the picture is opaque
          g_video[static_cast<size_t>(y) * w + x] = src[x] | 0xff000000u;
      }
      g_video_w = static_cast<int>(w);
      g_video_h = static_cast<int>(h);
    }
    int client_width() override { return 1280; }
    int client_height() override { return 720; }
    f64 client_display_rate() override { return 60.0; }
    bool has_alpha() override { return false; }
    // the handle type is a variant of window-system handles and headless has
    // none: the null backend never asks
    display_handle_t handle() const override { fmt::throw_exception("chimera: no display handle"); }
    // The GL renderer reads its flipped image back for a frame consumer (the
    // path RPCS3 records video through): with the bridge that consumer is
    // the frontend, and the image is the machine's picture for this frame.
    bool can_consume_frame() const override { return s_gpu; }
    void present_frame(std::vector<u8>&& data, u32 pitch, u32 width, u32 height, bool is_bgra) const override
    {
      if (!width || !height || width > 4096 || height > 4096 || data.size() < static_cast<size_t>(pitch) * height)
        return;
      g_video.assign(static_cast<size_t>(width) * height, 0);
      for (u32 y = 0; y < height; y++)
      {
        const u8* row = data.data() + static_cast<size_t>(y) * pitch;
        u32* dst = g_video.data() + static_cast<size_t>(y) * width;
        for (u32 x = 0; x < width; x++)
        {
          const u8* px = row + x * 4;
          // the frontend wants BGRA little-endian words
          dst[x] = is_bgra ? (u32{px[0]} | u32{px[1]} << 8 | u32{px[2]} << 16 | 0xff000000u)
                           : (u32{px[2]} | u32{px[1]} << 8 | u32{px[0]} << 16 | 0xff000000u);
        }
      }
      g_video_w = static_cast<int>(width);
      g_video_h = static_cast<int>(height);
    }
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
#ifdef CHIMERA_GL_BRIDGE
      case video_renderer::opengl:
        g_fxo->init<rsx::thread, named_thread<GLGSRender>>(ar);
        break;
#endif
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
    // the SPU decoder is part of the machine: the recompiler charges the
    // clock per function entry and per loop iteration (patch 0016) where the
    // interpreter charges per instruction, so the two machines keep slightly
    // different time. The setting decides; the runners may override it
    // through CHIMERA_SPU_DECODER for experiments.
    const char* spu = getenv("CHIMERA_SPU_DECODER");
    if (!spu)
      spu = s_spu_decoder;
    g_cfg.core.spu_decoder.set(!strcmp(spu, "asmjit") ? spu_decoder_type::asmjit : spu_decoder_type::_static);
    const char* ppu = getenv("CHIMERA_PPU_DECODER");
    if (!ppu)
      ppu = s_ppu_decoder;
    g_cfg.core.ppu_decoder.set(!strcmp(ppu, "llvm") ? ppu_decoder_type::llvm : ppu_decoder_type::_static);
    // a precompile session compiles for the recompiler whatever the run's
    // setting says: there is nothing else to precompile
    if (s_precompile_count > 0)
      g_cfg.core.ppu_decoder.set(ppu_decoder_type::llvm);
    // the LLVM recompilers, when built in: one compile thread (the machine's
    // scheduler is the only scheduler, and one order is one machine), a
    // fixed target CPU so the generated code is the same on every machine
    // that can run it, and no precompilation of every module at boot
    g_cfg.core.llvm_threads.set(1);
    g_cfg.core.llvm_cpu.from_string("x86-64-v3");
    g_cfg.core.llvm_precompilation.set(false);
    g_cfg.core.spu_cache.set(false);
    g_cfg.core.llvm_precompilation.set(false);
    g_cfg.core.spu_loop_detection.set(false);
    g_cfg.core.use_accurate_dfma.set(true);
    g_cfg.core.ppu_threads.set(2);
    g_cfg.core.clocks_scale.set(100);
    g_cfg.core.max_cpu_preempt_count_per_frame.set(0);
    g_cfg.core.thread_scheduler.set(thread_scheduler_mode::os);
    // the GL renderer only when asked for AND a GPU bridge was installed;
    // otherwise the null renderer, and the frontend is told (IsGpuActive)
    s_gpu = s_renderer_opengl && chimera_rpcs3_gpu_bridge_present && chimera_rpcs3_gpu_bridge_present();
    g_cfg.video.renderer.set(s_gpu ? video_renderer::opengl : video_renderer::null);
    if (s_gpu)
    {
      // one real context: shaders compile inline on the RSX thread, no
      // worker contexts, no async interpreter fallback
      g_cfg.video.shader_compiler_threads_count.set(0);
      g_cfg.video.shadermode.set(shader_mode::recompiler);
      // the flipped image is read back for the frame consumer every frame
      g_recording_mode = recording_mode::rpcs3;
    }
    g_cfg.video.frame_limit.set(frame_limit_type::_auto);
    g_cfg.video.vblank_rate.set(60);
    g_cfg.video.vblank_ntsc.set(false);
    g_cfg.video.multithreaded_rsx.set(false);
    g_cfg.audio.renderer.set(audio_renderer::null);
    // the null backend resolves no layout of its own, and the downmixer
    // refuses "automatic": two channels, the frontend's shape
    g_cfg.audio.channel_layout.set(audio_channel_layout::stereo);
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
    // The disc's own boot jingle, played by an OVERLAY and not by the machine.
    // It is not part of the emulated audio, it needs a video source the host
    // side of a headless build does not have, and asking for one is fatal
    // (overlay_audio.cpp ensure()s the callback) - which is how a game with a
    // SND0.AT3 on it killed the core before it drew a frame.
    g_cfg.misc.play_music_during_boot.set(false);
    // no firmware yet: the two startup libraries are HLE, nothing is loaded
    // from dev_flash (M3 turns this around)
    g_cfg.core.libraries_control.set_set({"liblv2.sprx:hle", "libsysmodule.sprx:hle"});
  }


  // The PS3 firmware, from Sony's PS3UPDAT.PUP, decrypted into /dev_flash of
  // the memory filesystem: rpcs3's own pipeline (pup_object -> the packages
  // TAR -> per package SCEDecrypter -> TAR extract through the VFS), which
  // upstream keeps behind a Qt dialog. Runs before the seal, so the firmware
  // is baseline, not savestate. Returns the version string, empty on failure.
  std::string install_firmware(const std::string& pup_path)
  {
    fs::file pup_f(pup_path);
    if (!pup_f)
    {
      fail("cannot open the firmware file " + pup_path);
      return {};
    }
    pup_object pup(std::move(pup_f));
    if (pup.operator pup_error() != pup_error::ok)
    {
      fail("the firmware file is not a valid PUP: " + pup.get_formatted_error());
      return {};
    }
    fs::file update_files_f = pup.get_file(0x300);
    if (!update_files_f || !update_files_f.size())
    {
      fail("the firmware file has no installation packages");
      return {};
    }
    std::string version;
    if (fs::file v = pup.get_file(0x100))
    {
      version = v.to_string();
      if (const usz nl = version.find('\n'); nl != umax)
        version.erase(nl);
    }
    tar_object update_files(update_files_f);
    auto names = update_files.get_filenames();
    std::erase_if(names, [](const std::string& n) { return n.find("dev_flash_") == umax; });
    if (names.empty())
    {
      fail("the firmware file has no dev_flash packages");
      return {};
    }
    // tar_object::extract writes through the VFS: /dev_flash must point at ours
    vfs::mount("/dev_flash", g_cfg_vfs.get_dev_flash());
    for (const auto& name : names)
    {
      auto stream = update_files.get_file(name);
      if (!stream)
      {
        fail("firmware package missing: " + name);
        return {};
      }
      if (stream->m_file_handler)
      {
        stream->m_file_handler->handle_file_op(*stream, 0, stream->get_size(umax), nullptr);
      }
      fs::file update_file = fs::make_stream(std::move(stream->data));
      SCEDecrypter self_dec(update_file);
      self_dec.LoadHeaders();
      self_dec.LoadMetadata(SCEPKG_ERK, SCEPKG_RIV);
      self_dec.DecryptData();
      auto dev_flash_tar_f = self_dec.MakeFile();
      if (dev_flash_tar_f.size() < 3)
      {
        fail("firmware package could not be decrypted: " + name);
        return {};
      }
      tar_object dev_flash_tar(dev_flash_tar_f[2]);
      if (!dev_flash_tar.extract())
      {
        fail("firmware package could not be extracted: " + name);
        return {};
      }
    }
    return version;
  }
}  // namespace

std::shared_ptr<PadHandlerBase> Chimera_MakePadHandler()
{
  return std::make_shared<chimera_pad_handler>();
}

void Chimera_PadPolled(u32)
{
  g_input_read = true;
}

void Chimera_AudioBlock(const f32* samples, u32 frames, u32 channels)
{
  for (u32 i = 0; i < frames; i++)
  {
    const f32 l = samples[i * channels];
    const f32 rr = channels > 1 ? samples[i * channels + 1] : l;
    g_audio_ring.push_back(static_cast<s16>(std::clamp(l, -1.0f, 1.0f) * 32767.0f));
    g_audio_ring.push_back(static_cast<s16>(std::clamp(rr, -1.0f, 1.0f) * 32767.0f));
  }
}

extern "C" {

const char* chimera_rpcs3_error(void)
{
  return g_error.c_str();
}

int chimera_rpcs3_init(const char* work_dir, const char* game_path, const char* firmware_path, const char* dkey_path)
{
  g_error.clear();
  // Everything the emulator reads or writes on its own lives in the memory
  // filesystem, identically in both flavors; the game is grafted into it
  // read-only from wherever the host put it (a path natively, a name in the
  // sandbox's file list). A work dir, when given, only receives the log.
  chimera::memfs_install();
  const std::string root = chimera::memfs_root + "/";
  g_android_executable_dir = root;
  g_android_config_dir = root + "config/";
  g_android_cache_dir = root + "cache/";
  chimera::memfs_mkdirs("config");
  chimera::memfs_mkdirs("cache");
  // the renderer's overlay icons (dialogs, the trophy notice), from the core
  chimera::memfs_mkdirs("config/Icons/ui");
  for (size_t i = 0; i < chimera_asset_count; i++)
    chimera::memfs_put(std::string("config/") + chimera_assets[i].path, reinterpret_cast<const char*>(chimera_assets[i].data), chimera_assets[i].size);
  const bool have_firmware = firmware_path && *firmware_path;
  if (!have_firmware)
  {
    // no firmware: the gate looks for this one file on the host path; an empty
    // file satisfies it, and the startup libraries are HLE so it is never read
    chimera::memfs_put("config/dev_flash/sys/external/liblv2.sprx", "", 0);
  }
  std::string game = game_path;
  {
    const char* base = strrchr(game_path, '/');
    base = base ? base + 1 : game_path;
    if (!chimera::memfs_graft(std::string("game/") + base, game_path))
    {
      fail(std::string("cannot open the game: ") + game_path);
      return 0;
    }
    game = root + "game/" + base;
    // a Redump disc key rides next to the ISO as "<stem>.dkey", where rpcs3's
    // ISO loader looks first (Loader/ISO.cpp: the path minus its extension)
    if (dkey_path && *dkey_path)
    {
      std::string stem = base;
      const size_t dot = stem.rfind('.');
      if (dot != std::string::npos)
        stem.resize(dot);
      if (!chimera::memfs_graft("game/" + stem + ".dkey", dkey_path))
      {
        fail(std::string("cannot open the disc key: ") + dkey_path);
        return 0;
      }
    }
  }

  // the emulator logs nowhere without a listener
  g_work = work_dir ? work_dir : "";
  if (!g_work.empty() && g_work.back() != '/')
    g_work += '/';
  if (!g_work.empty())
    fs::create_path(g_work);
  static std::unique_ptr<logs::listener> s_log = logs::make_file_listener((g_work.empty() ? root + "cache/" : g_work) + "RPCS3.log", 64 * 1024 * 1024);
  // fatal messages also reach the host's stderr: in the sandbox the log file
  // lives in memory nobody outside can read, and a thread that died is the
  // one thing worth hearing about (never machine state)
  struct fatal_to_stderr final : logs::listener
  {
    void log(u64, const logs::message& msg, std::string_view prefix, std::string_view text) override
    {
      if (static_cast<logs::level>(msg) == logs::level::fatal)
        fprintf(stderr, "rpcs3 fatal: %.*s %.*s\n", static_cast<int>(prefix.size()), prefix.data(), static_cast<int>(text.size()), text.data());
    }
  };
  static fatal_to_stderr s_fatal;
  static bool s_fatal_added = (logs::listener::add(&s_fatal), true);
  (void)s_fatal_added;

  // With CHIMERA_LOG_TRACE set, every message also reaches stderr: raising a
  // channel's LEVEL is no use on its own when the log it is raised into is a
  // file in the memory filesystem. (Native runner only - see below.)
  struct all_to_stderr final : logs::listener
  {
    void log(u64, const logs::message&, std::string_view prefix, std::string_view text) override
    {
      fprintf(stderr, "rpcs3: %.*s %.*s\n", static_cast<int>(prefix.size()), prefix.data(),
              static_cast<int>(text.size()), text.data());
    }
  };
  static all_to_stderr s_all;
  if (getenv("CHIMERA_LOG_TRACE"))
  {
    static bool s_all_added = (logs::listener::add(&s_all), true);
    (void)s_all_added;
  }

  vsched_init();

  Emu.SetHasGui(false);
  Emu.SetHeadless(true);
  Emu.SetSupportedRenderers({video_renderer::null, video_renderer::opengl});
  Emu.SetDefaultRenderer(video_renderer::null);
  Emu.SetCallbacks(make_callbacks());
  Emu.SetUsr("00000001");
  Emu.Init();

  pin_configuration();
  if (have_firmware)
  {
    // LLE the firmware's libraries the way a PS3 does
    g_cfg.core.libraries_control.set_set({});
    const std::string pup_rel = "firmware/" + std::string(strrchr(firmware_path, '/') ? strrchr(firmware_path, '/') + 1 : firmware_path);
    if (!chimera::memfs_graft(pup_rel, firmware_path))
    {
      fail(std::string("cannot open the firmware: ") + firmware_path);
      return 0;
    }
    g_firmware_version = install_firmware(root + pup_rel);
    if (g_firmware_version.empty())
      return 0;
    chimera_log.notice("firmware %s installed into the machine (%zu bytes of memory files)", g_firmware_version, chimera::memfs_bytes());
  }
  Emulator::SaveSettings(g_cfg.to_string(), "");

  g_tty_path = g_android_cache_dir + "TTY.log";

  // CHIMERA_LOG_TRACE=chan,chan (or "all"): those log channels at trace level,
  // for chasing a machine that goes quiet (the log is never machine state).
  // Applied again after the boot, because loading a game re-applies the
  // configured levels over the top of these.
  //
  // A sandboxed guest is handed no environment at all, so in the box the list
  // arrives as a mounted file called "logtrace" instead (run-wbx --log-trace).
  // CHIMERA_SPU_TRACE, still getenv, is native-only for that reason.
  const auto apply_log_trace = []
  {
    std::string list;
    if (const char* trace = getenv("CHIMERA_LOG_TRACE"))
    {
      list = trace;
    }
    else if (FILE* f = fopen("logtrace", "rb"))
    {
      // a sandboxed guest has no environment, so the host mounts the list as
      // a file instead (run-wbx --log-trace)
      char buf[256] = "";
      const size_t n = fread(buf, 1, sizeof buf - 1, f);
      fclose(f);
      while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[n - 1] = '\0';
      list = buf;
    }
    if (list.empty())
      return;
    size_t start = 0;
    while (start <= list.size())
    {
      size_t comma = list.find(',', start);
      if (comma == std::string::npos)
        comma = list.size();
      if (comma > start)
      {
        const std::string name = list.substr(start, comma - start);
        // "all" is every channel the build registered, which is what someone
        // chasing a machine that has gone quiet actually wants
        if (name == "all")
          for (const std::string& ch : logs::get_channels())
            logs::set_level(ch, logs::level::trace);
        else
          logs::set_level(name, logs::level::trace);
      }
      start = comma + 1;
    }
  };
  apply_log_trace();
  // one line for whoever runs the core, never machine state: which renderer
  // the machine got and why
  fprintf(stderr, "chimera rpcs3: renderer %s%s\n", s_gpu ? "opengl-hw through the GPU bridge" : "null",
          (!s_gpu && s_renderer_opengl) ? " (opengl-hw asked for, no GPU bridge offered)" : "");

  const game_boot_result r = Emu.BootGame(game, "", true, cfg_mode::custom);
  apply_log_trace();
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
  if (s_precompile_count > 0)
  {
    // A precompile session: no run. Every worker compiles its share of the
    // parts of the main executable and the modules it loaded (patch 0019:
    // the part's name decides whose it is), then sweeps the game's
    // directories (and the firmware's libraries) for every Nth file. Compiled objects reach the host through
    // the cache bridge as they finish; the driver thread pumps meanwhile.
    g_chimera_precompile_index = static_cast<u32>(s_precompile_index);
    g_chimera_precompile_count = static_cast<u32>(s_precompile_count);
    s_precompile_thread = std::make_unique<named_thread<std::function<void()>>>("Chimera Precompile"sv, []()
    {
      // every worker analyses and compiles its share of the parts of the
      // main executable and the modules it loaded (the part's name decides)
      ppu_initialize();
      std::vector<std::string> dirs;
      for (const std::string& d : Emu.GetGameDirs())
        dirs.emplace_back(d);
      if (s_precompile_firmware)
        dirs.emplace_back(vfs::get("/dev_flash/sys/external/"));
      chimera_ppu_precompile(dirs);
      s_precompile_done = true;
    });
    g_booted = true;
    return 1;
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
  g_input_read = false;
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
  // audio by machine time: 48000 / 60 = 800 frames per vblank (integer),
  // silence when the machine mixed less
  const size_t want = 800;
  g_audio_frame.assign(want * 2, 0);
  const size_t have = std::min(want * 2, g_audio_ring.size());
  std::copy(g_audio_ring.begin(), g_audio_ring.begin() + have, g_audio_frame.begin());
  g_audio_ring.erase(g_audio_ring.begin(), g_audio_ring.begin() + have);
}

void chimera_rpcs3_shutdown(void)
{
  if (!g_booted)
    return;
  g_booted = false;
  s_precompile_thread.reset();  // joins
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

uint8_t* chimera_rpcs3_main_memory_ptr(void)
{
  // the main block (0x00010000 for 0x0FFF0000) is mapped whole at init and
  // stays mapped: a direct view, no copy; the first 64 KiB are never mapped
  return vm::g_base_addr + 0x10000;
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

void chimera_rpcs3_set_port(int port, int present)
{
  if (port >= 0 && port < PORTS)
    g_port_present[port] = present != 0;
}

int chimera_rpcs3_port_present(int port)
{
  return port >= 0 && port < PORTS && g_port_present[port];
}

void chimera_rpcs3_set_button(int port, int index, int state)
{
  if (port >= 0 && port < PORTS && index >= 0 && index < BUTTONS)
    g_button[port][index] = state != 0;
}

void chimera_rpcs3_set_axis(int port, int index, int value)
{
  if (port >= 0 && port < PORTS && index >= 0 && index < AXES)
    g_axis[port][index] = static_cast<u8>(std::clamp(value, 0, 255));
}

int chimera_rpcs3_input_was_read(void)
{
  return g_input_read ? 1 : 0;
}

const uint32_t* chimera_rpcs3_video(int* w, int* h)
{
  *w = g_video_w;
  *h = g_video_h;
  return g_video.data();
}

const int16_t* chimera_rpcs3_audio(int* frames)
{
  *frames = static_cast<int>(g_audio_frame.size() / 2);
  return g_audio_frame.data();
}

const char* chimera_rpcs3_firmware_version(void)
{
  return g_firmware_version.c_str();
}

int chimera_rpcs3_is_running(void)
{
  return Emu.IsRunning() ? 1 : 0;
}

// Debugging: every CPU thread, where it is and what it last called, to
// stderr. For a machine that has gone quiet: a PPU parked in an lv2 wait
// names the HLE function it is parked in, which is usually the whole answer.
void chimera_rpcs3_debug_threads(void)
{
  fprintf(stderr, "== machine: status=%d time=%lluus vsched=%d threads switches=%llu\n",
          static_cast<int>(Emu.GetStatus(false)), (unsigned long long)(vsched_now_ns() / 1000),
          vsched_thread_count(), (unsigned long long)vsched_switch_count());
  idm::select<named_thread<ppu_thread>>([](u32 id, ppu_thread& p)
  {
    const auto name = p.ppu_tname.load();
    fprintf(stderr, "  PPU %08x cia=%08x state=%08x prio=%d in=%s last=%s name=%s\n",
            id, p.cia, static_cast<u32>(p.state.load()), p.prio.load().prio,
            p.current_function ? p.current_function : "-",
            p.last_function ? p.last_function : "-",
            name ? name->c_str() : "-");
    const std::string stack = p.dump_callstack();
    if (!stack.empty())
      fprintf(stderr, "%s\n", stack.c_str());
  });
  idm::select<named_thread<spu_thread>>([](u32 id, spu_thread& t)
  {
    fprintf(stderr, "  SPU %08x pc=%05x state=%08x status=%08x inbox=%u outbox=%u"
                    " tagmask=%08x tagstat=%u tagupd=%u mfcq=%u barrier=%08x fence=%08x"
                    " events=%08x stallmask=%08x\n",
            id, t.pc, static_cast<u32>(t.state.load()), t.status_npc.load().status,
            t.ch_in_mbox.get_count(), t.ch_out_mbox.get_count(),
            t.ch_tag_mask, t.ch_tag_stat.get_count(), t.ch_tag_upd,
            t.mfc_size, t.mfc_barrier, t.mfc_fence,
            static_cast<u32>(t.ch_events.load().events), t.ch_stall_mask);
    // the instructions around where it is parked: an SPU that is not moving is
    // almost always sitting on a channel read, and the opcode names which one
    for (int k = -2; k <= 2; k++)
    {
      const u32 at = (t.pc + k * 4) & 0x3fffc;
      const u32 w = *reinterpret_cast<const be_t<u32>*>(t.ls + at);
      fprintf(stderr, "    ls %05x: %08x  op11=%03x ra=%02x rt=%02x%s\n",
              at, w, w >> 21, (w >> 7) & 0x7f, w & 0x7f, k == 0 ? "  <== pc" : "");
    }
  });
  fflush(stderr);
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

extern "C" void chimera_rpcs3_set_renderer(const char* name)
{
  s_renderer_opengl = name && (strcmp(name, "opengl-hw") == 0 || strcmp(name, "opengl") == 0);
}

extern "C" int chimera_rpcs3_gpu_active(void)
{
  return s_gpu ? 1 : 0;
}

// A fault on a guest page: the renderer's caches protect pages of guest memory
// to learn of CPU writes, and this is how they learn. Natively the runner's
// SIGSEGV handler calls it; in the sandbox miniBox does (GuestFaultHandler).
// Mirrors the renderer part of RPCS3's own handle_access_violation.
static uint64_t g_faults_served;

extern "C" uint64_t chimera_rpcs3_fault_count(void)
{
  return g_faults_served;
}

extern "C" int chimera_rpcs3_on_fault(uint64_t addr, int is_write)
{
  // A fault the renderer declines kills the machine, and the reason is the
  // whole diagnosis. Say it, for the first few, whatever the log settings.
  static int s_declines = 0;
  const auto decline = [&](const char* why) -> int
  {
    if (s_declines++ < 8)
    {
      // write(2), not stdio: this runs inside the HOST's fault handler, where
      // musl's file lock reads a thread pointer that is not the guest's.
      char b[256]; int n = 0;
      for (const char *p = "chimera fault: declined 0x"; *p; p++) b[n++] = *p;
      for (int sh = 60; sh >= 0; sh -= 4) b[n++] = "0123456789abcdef"[(addr >> sh) & 0xf];
      for (const char *p = is_write ? " (write): " : " (read): "; *p; p++) b[n++] = *p;
      for (const char *p = why; *p && n < 250; p++) b[n++] = *p;
      b[n++] = '\n';
      ssize_t ig = write(2, b, n); (void)ig;
    }
    return 0;
  };

  const uint64_t base = reinterpret_cast<uint64_t>(vm::g_base_addr);
  if (addr < base || addr - base >= 0x1'0000'0000ull)
    return decline("outside the guest's 4 GiB view");
  const u32 vaddr = static_cast<u32>(addr - base);
  if (!rsx::g_access_violation_handler)
    return decline("the renderer has installed no handler");
  if (!vm::check_addr(vaddr))
    return decline("the machine says that address is not mapped");
  const auto cpu = get_current_cpu_thread();
  bool state_changed = false;
  if (cpu)
    state_changed = vm::temporary_unlock(*cpu);
  const bool handled = rsx::g_access_violation_handler(vaddr, is_write != 0);
  if (state_changed && (cpu->state += cpu_flag::temp, cpu->test_stopped()))
  {
    // the thread was asked to stop while it was away; nothing more to do here
  }
  if (handled)
    g_faults_served++;
  else
    decline("the renderer's caches do not own that page");
  return handled ? 1 : 0;
}

extern "C" void chimera_rpcs3_set_spu_decoder(const char* name)
{
  snprintf(s_spu_decoder, sizeof s_spu_decoder, "%s", name && !strcmp(name, "interpreter") ? "interpreter" : "asmjit");
}

extern "C" void chimera_rpcs3_set_ppu_decoder(const char* name)
{
  snprintf(s_ppu_decoder, sizeof s_ppu_decoder, "%s", name && !strcmp(name, "llvm") ? "llvm" : "interpreter");
}

// ---- the compile cache: the host keeps compiled objects between sessions ----
// (cache-bridge.h). Never machine state: an object is a pure function of the
// module, this package and the target CPU, so a warm run is a cold run minus
// the compile.
static chimera_cache_bridge_fn g_cache_bridge;
static uint64_t g_cache_fetched, g_cache_stored;

extern "C" void chimera_rpcs3_install_cache_bridge(uint64_t addr)
{
  g_cache_bridge = reinterpret_cast<chimera_cache_bridge_fn>(static_cast<uintptr_t>(addr));
}

extern "C" uint64_t chimera_rpcs3_cache_fetched(void)
{
  return g_cache_fetched;
}

extern "C" uint64_t chimera_rpcs3_cache_stored(void)
{
  return g_cache_stored;
}

// the name the host sees: the path under the emulator's cache directory
static std::string cache_key(const std::string& path)
{
  const std::string prefix = chimera::memfs_root + "/cache/";
  if (path.compare(0, prefix.size(), prefix) != 0)
    return {};
  return path.substr(prefix.size());
}

void Chimera_CacheFetch(const std::string& path)
{
  if (!g_cache_bridge)
    return;
  const std::string key = cache_key(path);
  if (key.empty() || fs::is_file(path))
    return;
  CacheFetchArgs args{};
  args.name = reinterpret_cast<uint64_t>(key.data());
  args.name_len = key.size();
  const uint64_t size = g_cache_bridge(CACHE_OP_FETCH, reinterpret_cast<uint64_t>(&args), 0, 0, 0, 0);
  if (!size)
    return;
  std::vector<u8> data(size);
  args.dst = reinterpret_cast<uint64_t>(data.data());
  args.cap = size;
  if (g_cache_bridge(CACHE_OP_FETCH, reinterpret_cast<uint64_t>(&args), 0, 0, 0, 0) != size)
    return;
  chimera::memfs_put(std::string("cache/") + key, data.data(), data.size());
  g_cache_fetched++;
}

void Chimera_CacheStore(const std::string& path)
{
  if (!g_cache_bridge)
    return;
  const std::string key = cache_key(path);
  if (key.empty())
    return;
  fs::file f(path, fs::read);
  if (!f)
    return;
  const std::vector<u8> data = f.to_vector<u8>();
  if (data.empty())
    return;
  CacheStoreArgs args{};
  args.name = reinterpret_cast<uint64_t>(key.data());
  args.name_len = key.size();
  args.data = reinterpret_cast<uint64_t>(data.data());
  args.size = data.size();
  if (g_cache_bridge(CACHE_OP_STORE, reinterpret_cast<uint64_t>(&args), 0, 0, 0, 0))
    g_cache_stored++;
}

// ---- precompile sessions -------------------------------------------------
extern "C" void chimera_rpcs3_set_precompile(int index, int count, int firmware_too)
{
  s_precompile_index = index;
  s_precompile_count = count;
  s_precompile_firmware = firmware_too != 0;
}

extern "C" int chimera_rpcs3_precompile_done(void)
{
  return s_precompile_done ? 1 : 0;
}

// files of the sweep done and in total, for a progress line
// Said from INSIDE the machine, as it happens: a compiling thread holds the
// scheduler for a whole module, so a frontend polling between frames sees the
// numbers jump at the end. Printed on change instead, this arrives live.
void chimera_precompile_changed()
{
  if (s_precompile_count <= 0)
    return;
  // modules, not parts: the total is the game's, counted before the queue was
  // split, and every session's done adds up to it
  static u32 s_last_done = ~0u, s_last_total = ~0u;
  if (g_chimera_precompile_done == s_last_done && g_chimera_precompile_total == s_last_total)
    return;
  s_last_done = g_chimera_precompile_done;
  s_last_total = g_chimera_precompile_total;
  // Through the cache bridge, which is the host's own code: a compiling thread
  // holds the scheduler for a whole module, so nobody can ask meanwhile, and
  // what this machine writes to its own streams is its own business.
  if (g_cache_bridge)
    g_cache_bridge(CACHE_OP_PROGRESS, s_last_done, s_last_total, 0, 0, 0);
  else
    fprintf(stderr, "Precompiled %u/%u modules\n", s_last_done, s_last_total);
}

extern "C" void chimera_rpcs3_precompile_progress(uint32_t* done, uint32_t* total)
{
  // this session's finished modules, and the game's module count - the same
  // total in every session, so the dones add up to it
  *done = g_chimera_precompile_done;
  *total = g_chimera_precompile_total;
}

// ---- Raw SPU MMIO, without a fault ----------------------------------------
//
// A Raw SPU shows its problem-state registers to the PPU as a window of memory
// at 0xE0000000 + index * 0x100000 + 0x40000. Upstream leaves that window
// UNMAPPED on purpose and services every access from inside its own SIGSEGV
// handler: it decodes the x64 instruction that faulted, performs the register
// access, and resumes with the instruction pointer moved past it.
//
// A sandboxed core cannot do that. The fault reaches the host, and the host's
// callback into the guest is told an address and a direction - it cannot
// rewrite the interrupted context, which is the whole trick. So the access is
// caught one level up instead, where the PPU makes it (vm::write and
// ppu_feed_data, patched), and never becomes a fault at all. The cost is one
// compare per guest memory access on the interpreter's path.
//
// Sizes: a register is four bytes and an access may not cross one, which is
// what upstream requires of the instruction it decodes. The value is the PPU's
// own, big-endian, and passes through unswapped - upstream's X64OP_*_BE cases,
// which are the ones a PowerPC load or store compiles to.
namespace {

constexpr bool raw_spu_is_mmio(u32 addr)
{
  return addr - RAW_SPU_BASE_ADDR < 6u * RAW_SPU_OFFSET
      && (addr % RAW_SPU_OFFSET) >= RAW_SPU_PROB_OFFSET;
}

spu_thread *raw_spu_for(u32 addr)
{
  const u32 index = (addr - RAW_SPU_BASE_ADDR) / RAW_SPU_OFFSET;
  const auto thread = idm::get_unlocked<named_thread<spu_thread>>(spu_thread::find_raw_spu(index));
  return thread ? static_cast<spu_thread *>(thread.get()) : nullptr;
}

}  // namespace

extern "C" bool chimera_rpcs3_raw_spu_mmio(u32 addr)
{
  return raw_spu_is_mmio(addr);
}

// Returns false when nothing owns the address, which leaves the caller to do
// what it would have done - and the fault, if there is one, to be reported.
extern "C" bool chimera_rpcs3_raw_spu_read(u32 addr, u32 size, u64 *out)
{
  if (!raw_spu_is_mmio(addr) || size > 4 || addr % 4 + size > 4) return false;
  spu_thread *const thread = raw_spu_for(addr);
  if (!thread) return false;
  // A read of these registers is a POLL - the PPU is asking whether the SPU has
  // got anywhere yet - and on a cooperative scheduler a poll that never gives
  // way is a deadlock: the SPU it is waiting for is a thread that only runs
  // when someone else stops. Natively the SPU has a core of its own and the
  // loop simply spins until it sees the answer. Here the loop IS the yield
  // point, and this is the only place it can be taken.
  vsched_yield(0);
  u32 value = 0;
  if (!thread->read_reg(addr & -4, value)) return false;
  // the register is four bytes; a narrower read takes its high-order end,
  // because the PPU is big-endian and so is this window
  value >>= (4 - size - (addr % 4)) * 8;
  *out = size == 4 ? value : (value & ((1u << (size * 8)) - 1));
  return true;
}

extern "C" bool chimera_rpcs3_raw_spu_write(u32 addr, u32 size, u64 value)
{
  if (!raw_spu_is_mmio(addr) || size != 4 || addr % 4) return false;
  spu_thread *const thread = raw_spu_for(addr);
  if (!thread) return false;
  return thread->write_reg(addr, static_cast<u32>(value));
}
