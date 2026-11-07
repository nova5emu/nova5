using System.Diagnostics;
using System.Text.RegularExpressions;

namespace Nova5.Services;

// Compiles a game's shaders and pipelines before it is played (precompile-windows.ps1 with kyty_shader_precompile.exe,
// both next to Nova5.exe): the emulator then takes them from _PipelineCache\static instead of compiling them on the
// way, which is what makes a game stutter for a moment at each new place. Quick: the shaders recorded play and the
// release's hints use (about half the time); full: every shader the game ships (hours the first time).
public static partial class ShaderPrep
{
	public static string Script => Path.Combine(AppContext.BaseDirectory, "precompile-windows.ps1");
	public static string Tool   => Path.Combine(AppContext.BaseDirectory, "kyty_shader_precompile.exe");
	public static bool   Available => File.Exists(Script) && File.Exists(Tool);

	// "  00:12:34 shards: 40 done, 3 running, 469 waiting (0 split)"
	[GeneratedRegex(@"shards: (\d+) done, (\d+) running, (\d+) waiting")]
	private static partial Regex ShardsLine();

	// Progress is the share of shards done; ready says the shaders of recorded play are in (the game can start).
	// False when the game ships no shaders the tool can find (a Unity game's are not in shader bundles).
	public static async Task<bool> RunAsync(Game game, bool quick, IProgress<(double Done, bool Ready)> progress, CancellationToken cancel)
	{
		if (!Available) throw new FileNotFoundException(L.T("Falta la herramienta de shaders (precompile-windows.ps1)"));
		var info = new ProcessStartInfo("powershell.exe")
		{
			UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true,
			WorkingDirectory = AppContext.BaseDirectory,
		};
		foreach (var arg in new[] { "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", Script, "-Game", game.Archive ?? game.Dir, "-Exe", Tool })
			info.ArgumentList.Add(arg);
		// Every CPU (the script's default reads a launch config Nova5 does not have); its shards run below normal priority.
		info.ArgumentList.Add("-Affinity");
		info.ArgumentList.Add((Environment.ProcessorCount >= 63 ? -1L : (1L << Environment.ProcessorCount) - 1).ToString());
		if (quick) info.ArgumentList.Add("-Quick");
		using var process = Process.Start(info) ?? throw new IOException(L.T("No se pudo iniciar la preparación de shaders"));
		// Everything the script says, for when it stops: logs\preparar-shaders-<title id>.txt next to Nova5.exe.
		var logPath = Path.Combine(AppContext.BaseDirectory, "logs", $"preparar-shaders-{game.TitleId}.txt");
		Directory.CreateDirectory(Path.GetDirectoryName(logPath)!);
		await using var log = new StreamWriter(logPath, append: false) { AutoFlush = true };
		var errors = process.StandardError.ReadToEndAsync(cancel);
		var ready  = false;
		var last   = "";
		var none   = false;
		try
		{
			while (await process.StandardOutput.ReadLineAsync(cancel) is { } line)
			{
				await log.WriteLineAsync($"{DateTime.Now:HH:mm:ss} {line}");
				if (line.Trim().Length > 0) last = line.Trim();
				if (line.Contains("ready to play")) ready = true;
				if (line.Contains("no shader bundles")) none = true;
				if (ShardsLine().Match(line) is { Success: true } m)
				{
					var done  = double.Parse(m.Groups[1].Value);
					var total = done + double.Parse(m.Groups[2].Value) + double.Parse(m.Groups[3].Value);
					progress.Report((total > 0 ? Math.Min(0.99, done / total) : 0, ready));
				}
				else if (ready)
				{
					progress.Report((-1, true));
				}
			}
			await process.WaitForExitAsync(cancel);
		}
		catch (OperationCanceledException)
		{
			await log.WriteLineAsync($"{DateTime.Now:HH:mm:ss} [Nova5] cancelado");
			process.Kill(entireProcessTree: true);
			throw;
		}
		var stderr = await errors;
		await log.WriteLineAsync($"{DateTime.Now:HH:mm:ss} [Nova5] terminó con el código {process.ExitCode}");
		if (stderr.Trim().Length > 0) await log.WriteLineAsync(stderr);
		if (process.ExitCode != 0)
		{
			var reason = stderr.Trim();
			throw new IOException(reason.Length > 0 ? reason.Split('\n')[^1].Trim() : last.Length > 0 ? last : L.F("la herramienta terminó con el código {0}", process.ExitCode));
		}
		progress.Report((1, true));
		return !none;
	}
}
