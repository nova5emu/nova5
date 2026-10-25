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
		var snapshot = Array.IndexOf(args, "--snapshot");
		if (snapshot >= 0 && snapshot + 1 < args.Length) MainWindow.SnapshotPath = Path.GetFullPath(args[snapshot + 1]);
		BuildAvaloniaApp().StartWithClassicDesktopLifetime(args);
	}

	public static AppBuilder BuildAvaloniaApp() => AppBuilder.Configure<App>().UsePlatformDetect();
}
