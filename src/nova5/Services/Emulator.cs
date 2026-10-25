using System.Diagnostics;

namespace Nova5.Services;

// Builds the Nova5Emulator command line from Nova5.ini.
public sealed class Emulator
{
	private const string Global = "GlobalConfiguration";

	public string  BaseDir { get; } = AppContext.BaseDirectory;
	public IniFile Ini     { get; }

	public Emulator()
	{
		var path   = Path.Combine(BaseDir, "Nova5.ini");
		var legacy = Path.Combine(BaseDir, "Kyty.ini");
		if (!File.Exists(path) && File.Exists(legacy)) File.Move(legacy, path); // Settings from before the rename.
		var fresh = !File.Exists(path);
		Ini = new IniFile(path);
		if (!fresh) return;
		// First run on this PC: console-like defaults, and CPU compatibility only where it is needed.
		Ini.Set("Launcher", "game_dirs", "");
		Ini.Set(Global, "fullscreen_enabled", "true");
		Ini.Set(Global, "screen_resolution", "R1920X1080");
		Ini.Set(Global, "shader_cache_enabled", "true");
		Ini.Set(Global, "perf_overlay_enabled", "false");
		Ini.Set(Global, "amd_cpu_enabled", NeedsCpuCompatibility() ? "true" : "false");
		Ini.Save();
	}

	// --amd-cpu emulates SSE4a/AMD-only instructions that PS5 games use. Intel CPUs lack SSE4a
	// (CPUID 0x80000001, ECX bit 6); without it some games crash right after the intro.
	public static bool NeedsCpuCompatibility()
	{
		if (!System.Runtime.Intrinsics.X86.X86Base.IsSupported) return false;
		var (_, _, ecx, _) = System.Runtime.Intrinsics.X86.X86Base.CpuId(unchecked((int)0x80000001), 0);
		return (ecx & (1 << 6)) == 0;
	}

	public string ExePath => Path.Combine(BaseDir, OperatingSystem.IsWindows() ? "Nova5Emulator.exe" : "Nova5Emulator");
	public bool   Exists  => File.Exists(ExePath);

	public IEnumerable<string> GameDirs() =>
	    (Ini.Get("Launcher", "game_dirs") ?? "")
	        .Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
	        .Select(d => d.Replace('/', Path.DirectorySeparatorChar));

	public bool ShaderCache => Ini.GetBool(Global, "shader_cache_enabled", true);

	public string? PatchFor(string titleId)
	{
		var patch = Path.Combine(BaseDir, "_Patches", titleId + ".json");
		return File.Exists(patch) ? patch : null;
	}

	public string? ShaderStoreFor(string titleId)
	{
		var store = Path.Combine(BaseDir, "_PipelineCache", titleId + ".shaders");
		return File.Exists(store) ? store : null;
	}

	public Process Launch(Game game)
	{
		var info = new ProcessStartInfo(ExePath) { WorkingDirectory = BaseDir, UseShellExecute = false };
		foreach (var arg in Arguments(game)) info.ArgumentList.Add(arg);
		return Process.Start(info) ?? throw new InvalidOperationException("Nova5Emulator did not start");
	}

	public List<string> Arguments(Game game)
	{
		var args = new List<string>();
		var resolution = (Get("screen_resolution", "R1920X1080")).TrimStart('R').Split('X');
		args.AddRange(["--screen-width", resolution[0], "--screen-height", resolution[1]]);
		args.AddRange(["--user-name", Get("user_name", "Nova5"), "--user-id", Get("user_id", "1000")]);
		Optional(args, "audio_input_device", "--mic");
		Optional(args, "controller_color", "--controller-color");
		args.AddRange(["--controller-volume", Get("controller_speaker_volume", "50")]);
		args.AddRange(["--controller-vibration", Get("controller_vibration_intensity", "100")]);
		args.AddRange(["--keyboard-player", Get("controller_keyboard_player", "0")]);
		args.AddRange(["--present-mode", Get("present_mode", "Mailbox")]);
		if (int.TryParse(Get("gpu_index", "-1"), out var gpu) && gpu >= 0) args.AddRange(["--gpu", gpu.ToString()]);
		Flag(args, "fullscreen_enabled", "--fullscreen");
		Flag(args, "hdr_enabled", "--hdr");
		Flag(args, "hide_cursor_enabled", "--hide-cursor");
		Flag(args, "readback_linear_images", "--readback-linear-images");
		Flag(args, "readback_tiled_images", "--readback-tiled-images");
		Flag(args, "sync_raw_image_buffers", "--sync-raw-image-buffers");
		if (!Ini.GetBool(Global, "trophy_enabled", true)) args.Add("--no-trophy-notifications");
		args.AddRange(["--trophy-duration", Get("trophy_duration_seconds", "6")]);
		Flag(args, "perf_overlay_enabled", "--perf-overlay");
		Flag(args, "skip_notice_screen", "--skip-notice-screen");
		Flag(args, "network_connected", "--network-connected");
		Flag(args, "tessellation_enabled", "--tessellation");
		Flag(args, "occlusion_queries_enabled", "--occlusion-queries");
		if (ShaderCache) args.Add("--shader-cache");
		args.AddRange(["--vblank-frequency", Get("vblank_frequency", "60")]);
		args.AddRange(["--console-language", Get("console_language", "1")]);
		Flag(args, "vulkan_validation_enabled", "--vulkan-validation");
		Flag(args, "shader_validation_enabled", "--shader-validation");
		args.AddRange(["--shader-optimization-type", Get("shader_optimization_type", "Performance")]);
		args.AddRange(["--shader-log-direction", Get("shader_log_direction", "Silent")]);
		args.AddRange(["--shader-log-folder", Get("shader_log_folder", "_Shaders")]);
		Flag(args, "command_buffer_dump_enabled", "--command-buffer-dump");
		args.AddRange(["--command-buffer-dump-folder", Get("command_buffer_dump_folder", "_Buffers")]);
		args.AddRange(["--printf-direction", Get("printf_direction", "Silent")]);
		args.AddRange(["--printf-output-file", Get("printf_output_file", "_nova5.txt")]);
		Flag(args, "profiler_enabled", "--profile");
		Flag(args, "amd_cpu_enabled", "--amd-cpu");
		Flag(args, "red_zone_protection_enabled", "--redzone");
		Flag(args, "renderdoc_enabled", "--rd");
		args.AddRange(["--game", game.Eboot]);
		if (PatchFor(game.TitleId) is { } patch) args.AddRange(["--game-patch", patch]);
		return args;
	}

	private string Get(string key, string fallback) =>
	    Ini.Get(Global, key) is { Length: > 0 } value ? value : fallback;

	private void Flag(List<string> args, string key, string flag)
	{
		if (Ini.GetBool(Global, key, false)) args.Add(flag);
	}

	private void Optional(List<string> args, string key, string flag)
	{
		if (Ini.Get(Global, key) is { Length: > 0 } value) args.AddRange([flag, value]);
	}
}
