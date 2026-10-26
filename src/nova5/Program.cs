using Avalonia;

namespace Nova5;

internal static class Program
{
	[STAThread]
	public static void Main(string[] args)
	{
		var musicTest = Array.IndexOf(args, "--music-test");
		if (musicTest >= 0 && musicTest + 2 < args.Length)
		{
			Environment.Exit(Services.MenuMusic.ExportWav(args[musicTest + 1], args[musicTest + 2]) ? 0 : 1);
		}
		if (args.Contains("--scan"))
		{
			// Lists what the library finds (games and packages waiting to install) without opening the UI.
			var emulator = new Services.Emulator();
			var roots    = emulator.GameDirs().Concat(Services.Store.Load().ExtraDirs).Distinct(StringComparer.OrdinalIgnoreCase);
			var lines    = Services.Library.Scan(roots).Select(g => $"{(g.Package is null ? "JUEGO" : "PKG  ")} {g.TitleId,-10} {g.Name} | {g.Package ?? g.Dir}");
			File.WriteAllLines(Path.Combine(AppContext.BaseDirectory, "_scan.txt"), lines);
			return;
		}
		var play = Array.IndexOf(args, "--play");
		if (play >= 0 && play + 1 < args.Length)
		{
			Environment.Exit(Play(args[play + 1]));
		}
		var snapshot = Array.IndexOf(args, "--snapshot");
		if (snapshot >= 0 && snapshot + 1 < args.Length) MainWindow.SnapshotPath = Path.GetFullPath(args[snapshot + 1]);
		BuildAvaloniaApp().StartWithClassicDesktopLifetime(args);
	}

	public static AppBuilder BuildAvaloniaApp() => AppBuilder.Configure<App>().UsePlatformDetect();

	// --play <game folder or eboot.bin>: starts the game with Nova5's settings and waits for it, so other
	// launchers (Nexo) get the same settings, patches and play-time tracking as the Nova5 UI.
	private static int Play(string target)
	{
		var dir  = File.Exists(target) ? Path.GetDirectoryName(Path.GetFullPath(target))! : Path.GetFullPath(target);
		var game = Services.Library.ReadGame(dir);
		if (game is null) return 2;
		var emulator = new Services.Emulator();
		if (!emulator.Exists) return 3;
		var started = DateTime.UtcNow;
		using var process = emulator.Launch(game);
		process.WaitForExit();
		var store = Services.Store.Load();
		store.PlaySeconds[game.TitleId] = store.PlaySeconds.GetValueOrDefault(game.TitleId) + (DateTime.UtcNow - started).TotalSeconds;
		store.LastPlayed[game.TitleId]  = DateTime.UtcNow;
		store.Save();
		return process.ExitCode;
	}
}
