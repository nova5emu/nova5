using System.Diagnostics;

namespace Nova5.Services;

// Installs PS5 packages (.pkg with the \x7FFIH header) found in the game folders, with the bundled
// pkg-to-anyps5 (GPL-3.0). The game lands beside the package in "<name>\app0" with its executables
// as ELF, which is what Nova5Emulator loads; the .pkg itself is left untouched.
public static class Packages
{
	// Present in the output folder while an install runs or after it failed; the library skips it.
	public const string Marker = ".nova5-installing";

	public static string Tool => Path.Combine(AppContext.BaseDirectory, "tools", "pkg-to-anyps5.exe");

	public static string OutputFor(string package) =>
	    Path.Combine(Path.GetDirectoryName(package)!, Path.GetFileNameWithoutExtension(package) + " (Nova5)");

	public static bool IsInstalled(string package) =>
	    File.Exists(Path.Combine(OutputFor(package), "app0", "eboot.bin")) && !File.Exists(Path.Combine(OutputFor(package), Marker));

	// Reads only sce_sys from the package (well under a second) into _Cache\pkg so the library can show the
	// game's title, cover, background and menu music before it is installed. Null when the package cannot be read.
	public static string? ReadInfo(string package)
	{
		var key   = Convert.ToHexString(System.Security.Cryptography.SHA1.HashData(System.Text.Encoding.UTF8.GetBytes(
		    package.ToLowerInvariant() + "|" + new FileInfo(package).Length)))[..16];
		var cache = Path.Combine(AppContext.BaseDirectory, "_Cache", "pkg", key);
		var app   = Path.Combine(cache, "app0");
		if (File.Exists(Path.Combine(app, "sce_sys", "param.json"))) return app;
		if (!File.Exists(Tool) || !LooksInstallable(package)) return null;
		try
		{
			if (Directory.Exists(cache)) Directory.Delete(cache, recursive: true);
			var info = new ProcessStartInfo(Tool) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
			foreach (var arg in new[] { package, cache, "--only", "sce_sys" }) info.ArgumentList.Add(arg);
			using var process = Process.Start(info);
			if (process is null) return null;
			_ = process.StandardOutput.ReadToEndAsync();
			_ = process.StandardError.ReadToEndAsync();
			if (!process.WaitForExit(60_000))
			{
				process.Kill(entireProcessTree: true);
				return null;
			}
			// Keep only what the library shows; the executables and the rest of sce_sys are not needed here.
			var source = Path.Combine(cache, "source");
			if (Directory.Exists(source)) Directory.Delete(source, recursive: true);
			var sceSys = Path.Combine(app, "sce_sys");
			string[] keep = { "param.json", "icon0.png", "pic0.png", "pic1.png", "snd0.at9" };
			foreach (var file in Directory.EnumerateFiles(sceSys, "*", SearchOption.AllDirectories).ToList())
			{
				if (!keep.Contains(Path.GetFileName(file)) || Path.GetDirectoryName(file) != sceSys) File.Delete(file);
			}
			foreach (var dir in Directory.GetDirectories(sceSys)) Directory.Delete(dir, recursive: true);
			return process.ExitCode == 0 && File.Exists(Path.Combine(app, "sce_sys", "param.json")) ? app : null;
		}
		catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.ComponentModel.Win32Exception)
		{
			return null;
		}
	}

	// Removes what a failed install left behind; only folders that still carry the marker are ours.
	public static void CleanUp(string package)
	{
		var output = OutputFor(package);
		if (File.Exists(Path.Combine(output, Marker))) Directory.Delete(output, recursive: true);
	}

	public static bool LooksInstallable(string package)
	{
		try
		{
			using var file = File.OpenRead(package);
			Span<byte> magic = stackalloc byte[4];
			return file.Read(magic) == 4 && magic.SequenceEqual(new byte[] { 0x7F, (byte)'F', (byte)'I', (byte)'H' });
		}
		catch (IOException)
		{
			return false; // Still being copied or locked.
		}
	}

	// A package is ready when its size has not changed for a few seconds (downloads and copies finish).
	public static async Task<bool> WaitUntilStableAsync(string package, CancellationToken cancel)
	{
		long last = -1;
		for (var stable = 0; stable < 3;)
		{
			await Task.Delay(2000, cancel);
			if (!File.Exists(package)) return false;
			var size = new FileInfo(package).Length;
			stable = size == last ? stable + 1 : 0;
			last   = size;
		}
		return true;
	}

	public static async Task InstallAsync(string package, IProgress<double> progress, CancellationToken cancel)
	{
		if (!File.Exists(Tool)) throw new FileNotFoundException("Falta el extractor de PKG (tools\\pkg-to-anyps5.exe)");
		if (!LooksInstallable(package)) throw new InvalidDataException("Este PKG no es un paquete de juego compatible (cifrado o dañado)");
		var size   = new FileInfo(package).Length;
		var output = OutputFor(package);
		var free   = new DriveInfo(Path.GetPathRoot(output)!).AvailableFreeSpace;
		if (free < size + (2L << 30)) throw new IOException($"No hay espacio: hacen falta {size / 1e9 + 2:0} GB libres");
		CleanUp(package); // A previous attempt that was interrupted starts over.
		if (Directory.Exists(output) && !File.Exists(Path.Combine(output, Marker)))
		{
			throw new IOException($"Ya existe la carpeta {output}; muévela o bórrala para instalar este PKG");
		}

		Directory.CreateDirectory(output);
		await File.WriteAllTextAsync(Path.Combine(output, Marker), package, cancel);
		var info = new ProcessStartInfo(Tool) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
		foreach (var arg in new[] { package, output, "--jobs", "4" }) info.ArgumentList.Add(arg);
		using var process = Process.Start(info) ?? throw new InvalidOperationException("No se pudo iniciar el extractor");
		var errors = process.StandardError.ReadToEndAsync(cancel);
		_ = process.StandardOutput.ReadToEndAsync(cancel);
		while (!process.HasExited)
		{
			await Task.Delay(1000, cancel);
			progress.Report(Math.Min(0.99, FolderSize(output) / (double)size));
		}
		if (process.ExitCode != 0)
		{
			throw new InvalidDataException("El extractor falló: " + (await errors).Trim().Split('\n').LastOrDefault());
		}

		// The emulator loads ELF executables: put the unwrapped ones from source\ over the packaged ones in app0\.
		var source = Path.Combine(output, "source");
		var app    = Path.Combine(output, "app0");
		if (Directory.Exists(source))
		{
			foreach (var file in Directory.EnumerateFiles(source, "*", SearchOption.AllDirectories))
			{
				var target = Path.Combine(app, Path.GetRelativePath(source, file));
				Directory.CreateDirectory(Path.GetDirectoryName(target)!);
				File.Copy(file, target, overwrite: true);
			}
			Directory.Delete(source, recursive: true);
		}
		if (!File.Exists(Path.Combine(app, "eboot.bin")) || !File.Exists(Path.Combine(app, "sce_sys", "param.json")))
		{
			throw new InvalidDataException("El paquete no contiene un juego (falta eboot.bin o param.json)");
		}
		File.Delete(Path.Combine(output, Marker));
		progress.Report(1);
	}

	private static long FolderSize(string dir)
	{
		try
		{
			return Directory.EnumerateFiles(dir, "*", SearchOption.AllDirectories).Sum(f => new FileInfo(f).Length);
		}
		catch (IOException)
		{
			return 0;
		}
	}
}
