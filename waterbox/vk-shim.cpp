// The guest end of Chimera's Vulkan bridge, for RPCS3's Vulkan renderer.
//
// The frontend hands the core one function (SetVulkanBridge, wbx-entry.cpp).
// miniBox's guest half (source/vk, linked into the core) turns every Vulkan
// command the renderer makes into a call of it. RPCS3 calls the commands by
// their own names, as functions a driver's library exports; in the box they
// are the entries of volk's table (waterbox/vk/volk.h), which is filled here
// by asking the guest half for each command by name.
//
// A load of the machine is a new Vulkan device on the frontend's side: the
// old one is destroyed there, with everything made on it. The renderer still
// has to let go of what it holds - handles, lists, memory - and it does that
// by destroying each object, as it would on a desktop. While it does, nothing
// crosses: every command answers "done" at once (the quiet switch), because
// the object it names is already gone and the frontend would refuse it.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include <vulkan/vulkan.h>
#include <volk.h>

extern "C" {
typedef uint64_t (*chimera_vk_bridge_fn)(uint64_t op, uint64_t a, uint64_t b, uint64_t c,
                                         uint64_t d, uint64_t e);
bool chimera_vk_install(chimera_vk_bridge_fn fn);
void* chimera_vk_lookup(const char* name);
uint64_t chimera_vk_context_id(void);
void chimera_vk_description(char* out, uint32_t size);
void chimera_vk_forget_mappings(void);
}

static chimera_vk_bridge_fn s_host;
static bool s_installed;
static bool s_quiet;

// the opcodes below 100 are the bridge's own questions (which device is this),
// and are answered whatever the switch says
static uint64_t Bridge(uint64_t op, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e)
{
  if (s_quiet && op >= 100)
    return 0;
  return s_host(op, a, b, c, d, e);
}

extern "C" void chimera_rpcs3_install_vulkan_bridge(uint64_t addr)
{
  s_host = reinterpret_cast<chimera_vk_bridge_fn>(static_cast<uintptr_t>(addr));
  s_installed = s_host && chimera_vk_install(Bridge);
  if (!s_installed)
    fprintf(stderr, "chimera: the Vulkan bridge was offered and refused\n");
}

extern "C" int chimera_rpcs3_vulkan_bridge_present(void)
{
  return s_installed;
}

// A COMMAND THE BRIDGE DOES NOT CARRY IS A NAMED STOP, NOT A JUMP TO NOTHING.
//
// volk asks for every command Vulkan has, and the bridge carries the ones on
// miniBox's list. The rest would be null entries in volk's table - and RPCS3
// calls a command by its name without asking whether it is there, because on
// a desktop a driver's library always has it. A call through a null entry is
// a jump to address 0 from inside the box, which ends the frontend, not the
// machine. So each missing command gets a function of its own that says
// which command it stands for and stops the machine there.
static const char* s_missing[1024];
static int s_missing_count;

[[noreturn]] static void MissingCalled(int index)
{
  fprintf(stderr, "chimera: the Vulkan renderer called %s, a command Chimera's Vulkan bridge does not carry\n",
          s_missing[index]);
  abort();
}

template <int N>
static void Missing()
{
  MissingCalled(N);
}

template <int... I>
static constexpr std::array<void (*)(), sizeof...(I)> MakeMissing(std::integer_sequence<int, I...>)
{
  return {{&Missing<I>...}};
}

static constexpr auto s_missing_fns = MakeMissing(std::make_integer_sequence<int, 1024>{});

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Lookup(VkInstance, const char* name)
{
  if (void* const found = chimera_vk_lookup(name))
    return reinterpret_cast<PFN_vkVoidFunction>(found);
  for (int i = 0; i < s_missing_count; i++)
    if (strcmp(s_missing[i], name) == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(s_missing_fns[i]);
  if (s_missing_count == 1024)
    return nullptr;
  // volk hands over string literals of its own: they stay where they are
  s_missing[s_missing_count] = name;
  return reinterpret_cast<PFN_vkVoidFunction>(s_missing_fns[s_missing_count++]);
}

// Fills volk's table: with no instance, the commands that need none; with
// one, all of them (vkutils/instance.cpp).
extern "C" bool chimera_vk_load_commands(VkInstance instance)
{
  if (!s_installed)
    return false;
  if (instance == VK_NULL_HANDLE)
    volkInitializeCustom(Lookup);
  else
    volkLoadInstance(instance);
  return true;
}

// which device the frontend has now; 0 when it cannot say
extern "C" uint64_t chimera_rpcs3_vulkan_context(void)
{
  return s_installed ? chimera_vk_context_id() : 0;
}

// the card, in words, for the log
extern "C" const char* chimera_rpcs3_vulkan_describe(void)
{
  static char text[256];
  text[0] = 0;
  if (s_installed)
    chimera_vk_description(text, sizeof text);
  return text;
}

// A LOAD IS THE HOST'S WORD (wbx-entry.cpp, StateLoaded): the renderer keeps
// the device its objects came from beside them, and a state taken before it
// first looked carries 0 there, which says nothing about what the objects
// are. Taken and cleared by the renderer when it next looks.
static int s_state_loaded;

// The switch goes on here, at the load itself, and not when the renderer
// notices: the renderer is not the only thread that calls Vulkan. RPCS3's
// "Vulkan Driver Manager" thread frees objects the card has finished with,
// and after a load it can run before the RSX thread has made the renderer
// again - handing the frontend objects of the device that is gone, which the
// frontend refuses with "device lost", which RPCS3 takes for the end
// (Dead or Alive 5, a load on frame 1700). From the load until the renderer
// has let go of everything, every command is answered "done" and none crosses.
extern "C" void chimera_rpcs3_vulkan_note_state_loaded(void)
{
  s_state_loaded = 1;
  if (s_installed)
    s_quiet = true;
}

extern "C" int chimera_rpcs3_vulkan_take_state_loaded(void)
{
  const int was = s_state_loaded;
  s_state_loaded = 0;
  return was;
}

// The renderer is about to let go of a device a load destroyed...
extern "C" void chimera_rpcs3_vulkan_quiet(void)
{
  s_quiet = true;
}

// ...and has: the copies of mapped memory nothing will unmap now are given
// back, and commands cross again for the device about to be made.
extern "C" void chimera_rpcs3_vulkan_old_device_gone(void)
{
  if (!s_quiet)
    return;
  chimera_vk_forget_mappings();
  s_quiet = false;
}
