using System.Diagnostics;
using System.Globalization;
using System.Text.RegularExpressions;

namespace Nova5.Services;

// Installs PS5 packages (.pkg with the \x7FFIH header) found in the game folders, with the bundled
// pkg-to-anyps5 (GPL-3.0). The game lands beside the package in "<name>\app0" with its executables
// as ELF, which is what Nova5Emulator loads; the .pkg itself is left untouched.
public static partial class Packages
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

	// Where an install is. Fraction is null while the length is unknown (waiting, reading the package index);
	// the byte, speed and file figures are those of the copy, the long step.
	public sealed record InstallProgress(string Step, double? Fraction, long Done = 0, long Total = 0, double Speed = 0,
	                                     TimeSpan? Remaining = null, long FilesDone = 0, long FilesTotal = 0);

	public static async Task InstallAsync(string package, IProgress<InstallProgress> progress, CancellationToken cancel)
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
		progress.Report(new InstallProgress("Leyendo el paquete…", null));
		var info = new ProcessStartInfo(Tool)
		{
			UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true,
			StandardOutputEncoding = System.Text.Encoding.UTF8, StandardErrorEncoding = System.Text.Encoding.UTF8,
		};
		foreach (var arg in new[] { package, output, "--jobs", "4", "--verbose" }) info.ArgumentList.Add(arg);
		using var process = Process.Start(info) ?? throw new InvalidOperationException("No se pudo iniciar el extractor");
		var errors = process.StandardError.ReadToEndAsync(cancel);
		var status = new ExtractorStatus();
		var lines  = Task.Run(async () =>
		{
			while (await process.StandardOutput.ReadLineAsync(cancel) is { } line) status.Read(line);
		}, cancel);
		while (!process.HasExited)
		{
			await Task.Delay(500, cancel);
			if (status.Total + (2L << 30) > free) // The package is compressed: the real size is known once its index is read.
			{
				process.Kill(entireProcessTree: true);
				throw new IOException($"No hay espacio: el juego ocupa {status.Total / (double)(1L << 30):0} GB y hacen falta {status.Total / (double)(1L << 30) + 2:0} GB libres");
			}
			progress.Report(status.Now());
		}
		await lines;
		if (process.ExitCode != 0)
		{
			throw new InvalidDataException("El extractor falló: " + (await errors).Trim().Split('\n').LastOrDefault());
		}
		progress.Report(new InstallProgress("Terminando…", null));

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
		progress.Report(new InstallProgress("Instalado", 1));
	}

	// Follows pkg-to-anyps5 --verbose without a terminal. It prints a line when a step starts:
	//   » [3/3] copying app0: 223,246 files, 81.9 GiB
	// and every 5 s while one with a length runs:
	//   copying app0  45.3%  37.1/81.9 GiB  210 MiB/s  ETA 3:12  100,000/223,246 files
	// Between those lines the copy is advanced at the last speed, so the bar keeps moving.
	private sealed partial class ExtractorStatus
	{
		private readonly object _lock = new();
		private string   _step = "Leyendo el paquete…";
		private bool     _copying;
		private double   _fraction, _speed;
		private long     _done, _total, _filesDone, _filesTotal;
		private TimeSpan? _remaining;
		private DateTime _at = DateTime.UtcNow;

		// The size of the whole game, from the package index (the "files" line) or else from the copy step.
		public long Total { get { lock (_lock) return _game > 0 ? _game : _copying ? _total : 0; } }

		private long _game;

		public void Read(string line)
		{
			lock (_lock)
			{
				if (GameLine().Match(line) is { Success: true } game)
				{
					_game = Bytes(game.Groups[1].Value, game.Groups[2].Value);
				}
				else if (StepLine().Match(line) is { Success: true } step)
				{
					_copying  = step.Groups[1].Value == "copying app0";
					_step     = step.Groups[1].Value switch
					{
						"reading package"        => "Leyendo el paquete…",
						"unwrapping executables" => "Preparando los ejecutables…",
						"copying app0"           => "Copiando archivos",
						_                        => "Instalando…",
					};
					_fraction   = 0;
					_done       = 0;
					_speed      = 0;
					_remaining  = null;
					_filesDone  = 0;
					_filesTotal = step.Groups[2].Success ? Count(step.Groups[2].Value) : 0;
					_total      = step.Groups[3].Success ? Bytes(step.Groups[3].Value, step.Groups[4].Value) : 0;
					_at         = DateTime.UtcNow;
				}
				else if (ProgressLine().Match(line) is { Success: true } p)
				{
					_fraction  = Number(p.Groups[1].Value) / 100;
					_done      = Bytes(p.Groups[2].Value, p.Groups[4].Value);
					_total     = Bytes(p.Groups[3].Value, p.Groups[4].Value);
					_speed     = p.Groups[5].Value == "0 B" ? 0 : Bytes(p.Groups[5].Value.Split(' ')[0], p.Groups[5].Value.Split(' ')[1]);
					_remaining = TimeSpan.TryParseExact(p.Groups[6].Value, [@"m\:ss", @"h\:mm\:ss"], CultureInfo.InvariantCulture, out var eta) ? eta : null;
					_filesDone  = p.Groups[7].Success ? Count(p.Groups[7].Value) : 0;
					_filesTotal = p.Groups[8].Success ? Count(p.Groups[8].Value) : _filesTotal;
					_at        = DateTime.UtcNow;
				}
			}
		}

		public InstallProgress Now()
		{
			lock (_lock)
			{
				if (!_copying) return new InstallProgress(_step, _step == "Preparando los ejecutables…" && _total > 0 ? _fraction : null);
				var since     = (DateTime.UtcNow - _at).TotalSeconds;
				var done      = _total > 0 ? Math.Min(_total, _done + (long)(_speed * since)) : _done;
				var remaining = _remaining is { } r ? TimeSpan.FromSeconds(Math.Max(0, r.TotalSeconds - since)) : (TimeSpan?)null;
				var fraction  = _total > 0 ? Math.Min(0.99, done / (double)_total) : _fraction;
				return new InstallProgress(_step, fraction, done, _total, _speed, remaining, _filesDone, _filesTotal);
			}
		}

		private static double Number(string text) => double.Parse(text, CultureInfo.InvariantCulture);
		private static long   Count(string text)  => long.Parse(text.Replace(",", ""), CultureInfo.InvariantCulture);

		private static long Bytes(string value, string unit) => (long)(Number(value) * unit switch
		{
			"KiB" => 1L << 10, "MiB" => 1L << 20, "GiB" => 1L << 30, "TiB" => 1L << 40, _ => 1L,
		});

		[GeneratedRegex(@"^» \[\d+/\d+\] ([a-z0-9 ]+?)(?:: ([\d,]+) files, ([\d.]+) (B|KiB|MiB|GiB|TiB))?$")]
		private static partial Regex StepLine();

		[GeneratedRegex(@"^files\s+[\d,]+ under uroot, ([\d.]+) (B|KiB|MiB|GiB|TiB)$")]
		private static partial Regex GameLine();

		[GeneratedRegex(@"^\s+[a-z0-9 ]+?\s{2}([\d.]+)%\s{2}([\d.]+)/([\d.]+) (B|KiB|MiB|GiB|TiB)\s{2}(0 B|[\d.]+ (?:B|KiB|MiB|GiB|TiB))/s\s{2}ETA (\S+)(?:\s{2}([\d,]+)/([\d,]+) files)?$")]
		private static partial Regex ProgressLine();
	}
}
