/**
 * @file        ui/windowed_app_main_switch.cpp
 * @brief       Application entry point on Horizon.
 *
 * @license     BSD 3-Clause License
 */

#include <rex/platform.h>

#if REX_PLATFORM_SWITCH

#include <switch.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/logging/api.h>
#include <rex/main_switch.h>
#include <rex/ui/windowed_app.h>
#include <rex/ui/windowed_app_context_switch.h>

namespace {

constexpr const char* kAppRoot = "sdmc:/switch/skate3";

// Reads one argument per line, ignoring blanks and # comments, and lets each
// replace a shipped default for the same key rather than sit beside it: CLI11
// rejects a scalar option it sees twice, and that failure discards everything.
std::vector<std::string> ApplyArgumentFileOverrides(std::vector<std::string> args,
                                                    const std::filesystem::path& override_file) {
  std::ifstream in(override_file);
  if (!in) {
    return args;
  }

  std::vector<std::string> overrides;
  std::string line;
  while (std::getline(in, line)) {
    // Trim both ends; a stray carriage return from a desktop editor would
    // otherwise become part of the value.
    const auto first = line.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
      continue;
    }
    const auto last = line.find_last_not_of(" \t\r\n");
    line = line.substr(first, last - first + 1);
    if (line.empty() || line[0] == '#') {
      continue;
    }
    if (line.rfind("--", 0) != 0) {
      line = "--" + line;
    }
    overrides.push_back(line);
  }
  if (overrides.empty()) {
    return args;
  }

  auto key_of = [](const std::string& arg) {
    std::string_view key(arg);
    key.remove_prefix(2);
    return std::string(key.substr(0, key.find('=')));
  };

  for (const std::string& override_arg : overrides) {
    const std::string key = key_of(override_arg);
    std::erase_if(args, [&](const std::string& shipped) { return key_of(shipped) == key; });
    // The player asked for this, so it outranks anything settings.toml holds.
    rex::cvar::NoteExplicitlySet(key);
  }
  args.insert(args.end(), overrides.begin(), overrides.end());
  std::fprintf(stderr, "[args] %zu override(s) from %s\n", overrides.size(),
               override_file.c_str());
  return args;
}

// The caches that hold decoded textures and meshes. Sized from what the kernel
// says this process actually got, because a retail console and an 8 GB
// development unit are the same code with very different room.
//
// The numbers matter more than they look. A dense area of this game has a
// measured working set of roughly 400 MB of textures and 280 MB of meshes;
// below that the caches do not merely miss more often, they evict something
// still in view every frame and the streaming never converges. An earlier
// Switch port shipped 192/128 and was unplayable for exactly this reason.
void AppendStoreBudgets(std::vector<std::string>& args) {
  const uint64_t total = rex::SwitchTotalMemory();
  const bool expanded = total >= (5ull << 30);  // an 8 GB unit after reservations
  const int tex_mb = expanded ? 512 : 400;
  const int mesh_mb = expanded ? 384 : 280;
  args.push_back("--skate3_native_render_scene_tex_store_mb=" + std::to_string(tex_mb));
  args.push_back("--skate3_native_render_scene_mesh_store_mb=" + std::to_string(mesh_mb));
  std::fprintf(stderr, "[args] pool %llu MiB -> tex store %d MB, mesh store %d MB\n",
               (unsigned long long)(total >> 20), tex_mb, mesh_mb);
}

std::vector<std::string> BuildSwitchArguments() {
  const std::filesystem::path root(kAppRoot);
  std::vector<std::string> args{
      // --- where things live ------------------------------------------------
      "--game_data_root=" + (root / "game").string(),
      "--user_data_root=" + (root / "user").string(),
      "--log_file=" + (root / "skate3.log").string(),
      // A crash loses whatever is still buffered, and on a console every
      // failure is someone else's crash report.
      "--log_flush_interval=1",
      "--log_level=warn",

      // --- frame pacing -----------------------------------------------------
      // Three Cortex-A57 cores at roughly a gigahertz cannot hold 60. An even
      // 30 is worth more than an uneven 40: the guest produces frames at
      // irregular intervals and a 60 Hz display beat-samples them into judder.
      "--skate3_guest_fps_cap=30",
      "--skate3_guest_fps_cap_auto=false",
      // No spinning at the tail of the pace. Everything here runs at the one
      // priority where equal-priority threads are time-sliced, so a spin holds
      // its core against every sibling on it - and there are only three.
      "--skate3_guest_fps_cap_spin_us=0",
      "--vsync=true",

      // --- the renderer -----------------------------------------------------
      "--skate3_native_render_scene=true",
      // Menus, loading screens and the lightmap and outfit composition passes
      // still go through the emulated path, and the native renderer samples
      // their output out of guest memory. Mode 2 is what keeps those running.
      "--native_render_suppress_mode=2",
      "--resolution_scale=1",
      "--draw_resolution_scale_x=1",
      "--draw_resolution_scale_y=1",
      // Everything optional starts off. Turn them on one at a time and read the
      // pace line; none of this is worth a frame until the frame is steady.
      "--skate3_native_render_scene_msaa=1",
      "--skate3_native_render_scene_ssao=false",
      "--skate3_native_render_scene_bloom=false",
      "--skate3_native_render_scene_shafts=false",
      "--skate3_native_render_scene_shadow_pcss=false",
      "--skate3_native_render_scene_shadow_static_size=1024",
      "--skate3_draw_distance_scale=1.0",
      "--skate3_lod_distance_scale=1.0",
      "--texture_cache_memory_limit_soft=128",
      "--texture_cache_memory_limit_hard=192",

      // --- spin caps --------------------------------------------------------
      // These are the levers that stopped the Android command processor being
      // runnable with no core to run on. The reasoning applies with more force
      // to three cores than to eight.
      "--gpu_idle_spin_iterations=0",
      "--rtl_critical_section_max_spin=64",
      "--gpu_wait_reg_mem_timeout_ms=20",
      // Pipelines are compiled by NVK on this CPU. More threads than one only
      // takes cores away from the frame that is waiting for them.
      "--vulkan_pipeline_creation_threads=1",

      // --- audio ------------------------------------------------------------
      "--audio_device_channels=2",
      "--audio_device_sample_frames=512",

      // --- input ------------------------------------------------------------
      // There is a touchscreen, but there are also always buttons. The overlay
      // is off unless the player asks for it.
      "--touch_controls=false",

      // --- thread placement -------------------------------------------------
      // Core 3 belongs to the system; an application gets 0, 1 and 2. Priority
      // 59 is the only level on those cores where equal-priority threads are
      // time-sliced, so everything carrying guest work or spinning sits there.
      "--switch_thread_placement_map="
      "Main XThread=core:0,prio:59;"
      "render_thread=core:1,prio:59;"
      "GPU Commands=core:2,prio:59;"
      "XMA Decoder=core:2,prio:59;"
      "Vulkan Pipelines=core:2,prio:59;"
      "load_thread=core:2,prio:59;"
      "rwfilesys=core:2,prio:59;"
      "presence_thread=core:2,prio:59;"
      "decode_worker=core:any,prio:59;"
      "cam_sampler=core:any,prio:59;"
      "RwAudioCore=core:any,prio:46;"
      "Audio Worker=core:any,prio:45;"
      "GPU VSync=core:any,prio:50;"
      "Kernel Dispatch=core:any,prio:55",
  };

  AppendStoreBudgets(args);
  return ApplyArgumentFileOverrides(std::move(args), root / "user" / "switch_args.txt");
}

}  // namespace

int main(int argc, char** argv) {
  // Brings up the SD card, the pads and the network, and refuses to continue in
  // applet mode - where this process would get a fraction of the memory and
  // none of the syscalls the guest address space is built from. It explains
  // itself on the way out.
  if (!rex::InitializeSwitchApp()) {
    return EXIT_FAILURE;
  }

  // NVK refuses to create a device without this. It is read when the instance
  // is created, so it has to be set before anything touches Vulkan.
  setenv("NVK_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1);

  {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(kAppRoot) / "user", ec);
    std::filesystem::create_directories(std::filesystem::path(kAppRoot) / "cache", ec);
    // stderr was pointed at its file by InitializeSwitchApp, before it wrote
    // anything. Redirecting again here would truncate that away.
  }

  // What the launcher passed is the operator's intent: record it as explicitly
  // set, and let it replace the shipped default for the same key rather than
  // sit beside it, because CLI11 rejects a scalar option it sees twice and
  // that failure discards every argument.
  std::vector<std::string> launcher_keys;
  for (int i = 1; i < argc; ++i) {
    std::string_view arg(argv[i]);
    if (arg.rfind("--", 0) != 0) {
      continue;
    }
    arg.remove_prefix(2);
    const std::string_view key = arg.substr(0, arg.find('='));
    launcher_keys.emplace_back(key);
    rex::cvar::NoteExplicitlySet(key);
  }

  std::vector<std::string> switch_args = BuildSwitchArguments();
  std::erase_if(switch_args, [&](const std::string& shipped) {
    std::string_view key(shipped);
    key.remove_prefix(2);
    key = key.substr(0, key.find('='));
    return std::find(launcher_keys.begin(), launcher_keys.end(), key) != launcher_keys.end();
  });

  std::vector<char*> spliced_argv;
  spliced_argv.reserve(size_t(argc) + switch_args.size());
  spliced_argv.push_back(argc > 0 ? argv[0] : const_cast<char*>("skate3"));
  for (const std::string& arg : switch_args) {
    spliced_argv.push_back(const_cast<char*>(arg.c_str()));
  }
  for (int i = 1; i < argc; ++i) {
    spliced_argv.push_back(argv[i]);
  }
  argc = int(spliced_argv.size());
  argv = spliced_argv.data();

  auto remaining = rex::cvar::Init(argc, argv);
  rex::cvar::ApplyEnvironment();
  rex::InitLoggingEarly();

  int result = EXIT_FAILURE;
  {
    rex::ui::SwitchWindowedAppContext app_context;
    if (!app_context.Initialize()) {
      std::fprintf(stderr, "Failed to initialize the applet loop\n");
      rex::ShutdownSwitchApp();
      return EXIT_FAILURE;
    }

    std::unique_ptr<rex::ui::WindowedApp> app = rex::ui::GetWindowedAppCreator()(app_context);

    const auto& option_names = app->GetPositionalOptions();
    std::map<std::string, std::string> parsed;
    const size_t count = std::min(remaining.size(), option_names.size());
    for (size_t i = 0; i < count; ++i) {
      parsed[option_names[i]] = remaining[i];
    }
    app->SetParsedArguments(std::move(parsed));

    const bool initialized = app->OnInitialize();
    result = initialized ? app_context.RunMainLoop() : EXIT_FAILURE;
  }

  // Deliberately no teardown of the app or the runtime, for the same reason as
  // macOS and Android: guest threads cannot be reliably stopped - Horizon has
  // nothing like pthread_cancel, and recompiled code never reaches a
  // cancellation point - so the destructor chain would race still-running
  // threads over freed kernel objects. Flush the logs and leave.
  rex::FlushLogging();
  rex::ShutdownSwitchApp();
  std::_Exit(result);
}

#endif  // REX_PLATFORM_SWITCH
