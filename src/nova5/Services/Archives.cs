using System.Collections.Concurrent;
using System.Diagnostics;
using System.Text.RegularExpressions;
using SharpCompress.Archives;
using SharpCompress.Common;
using SharpCompress.Readers;

namespace Nova5.Services;

// Unpacks game archives (.rar, .zip, .7z, and split sets such as "name.part1.rar") found in the game folders, with the
// bundled SharpCompress (MIT). The content lands beside the archive in "<name> (Nova5)"; the archive itself is left
// untouched. Only archives holding a PS5 game (eboot.bin and sce_sys/param.json) or a .pkg are taken, so other
// archives in the game folders are left alone. A .pkg that comes out of one is then installed like any other.
public static partial class Archives
{
	public static readonly string[] Patterns = { "*.rar", "*.zip", "*.7z" };

	public static bool IsArchive(string path) =>
	    Path.GetExtension(path).ToLowerInvariant() is ".rar" or ".zip" or ".7z";

	// The tile's label: RAR, ZIP or 7Z.
	public static string Label(string archive) => Path.GetExtension(archive).TrimStart('.').ToUpperInvariant();

	// Later parts of a split set ("name.part2.rar") are read through the first one.
	public static bool IsFirstVolume(string archive) =>
	    PartNumber().Match(Path.GetFileName(archive)) is not { Success: true } part || int.Parse(part.Groups[1].Value) == 1;

	public static string OutputFor(string archive) =>
	    Path.Combine(Path.GetDirectoryName(archive)!, BaseName(archive) + " (Nova5)");

	// The name without the part number and extension: "Game.part1.rar" -> "Game".
	public static string BaseName(string archive) => PartNumber().Replace(Path.GetFileName(archive), "") is var name &&
	    name != Path.GetFileName(archive) ? name : Path.GetFileNameWithoutExtension(archive);

	public static bool IsExtracted(string archive) =>
	    Directory.Exists(OutputFor(archive)) && !File.Exists(Path.Combine(OutputFor(archive), Packages.Marker));

	public static void CleanUp(string archive)
	{
		var output = OutputFor(archive);
		if (File.Exists(Path.Combine(output, Packages.Marker))) Directory.Delete(output, recursive: true);
	}

	// What an archive holds, read once per session (keyed by its parts' sizes and dates, so a set still being copied
	// is read again once it grows).
	private sealed record Contents(bool HoldsGame, string? Info);

	private static readonly ConcurrentDictionary<string, Contents> Known = new(StringComparer.OrdinalIgnoreCase);

	public static bool HoldsGame(string archive) => Read(archive).HoldsGame;

	// The game's sce_sys (param.json, icon, background, music) copied into _Cache\archive, so the library can show the
	// title and art before it is unpacked. Null when the archive is solid (reading them would mean unpacking it all).
	public static string? ReadInfo(string archive) => Read(archive).Info;

	private static Contents Read(string archive)
	{
		var parts = Parts(archive);
		var key   = string.Join("|", parts.Select(p => p.FullName + ":" + p.Length + ":" + p.LastWriteTimeUtc.Ticks));
		return Known.GetOrAdd(key, _ => Inspect(archive, parts));
	}

	private static List<FileInfo> Parts(string archive)
	{
		try
		{
			return ArchiveFactory.GetFileParts(new FileInfo(archive)).ToList();
		}
		catch (Exception e) when (e is IOException or ArgumentException or NotSupportedException or InvalidOperationException)
		{
			return new List<FileInfo> { new(archive) };
		}
	}

	private static Contents Inspect(string archive, List<FileInfo> parts)
	{
		try
		{
			using var opened = ArchiveFactory.OpenArchive(parts, new ReaderOptions());
			string? eboot = null, param = null;
			var hasPackage = false;
			foreach (var entry in opened.Entries)
			{
				if (entry.IsDirectory || entry.Key is not { } name) continue;
				name = name.Replace('\\', '/');
				if (name.EndsWith(".pkg", StringComparison.OrdinalIgnoreCase)) hasPackage = true;
				else if (Path.GetFileName(name).Equals("eboot.bin", StringComparison.OrdinalIgnoreCase)) eboot ??= Folder(name);
				else if (name.EndsWith("sce_sys/param.json", StringComparison.OrdinalIgnoreCase)) param ??= Folder(Folder(name));
				if (hasPackage || (eboot is not null && eboot == param)) break;
			}
			var game = eboot is not null && eboot == param;
			if (!game && !hasPackage) return new Contents(false, null);
			return new Contents(true, game && !opened.IsSolid ? CacheInfo(archive, parts, opened, eboot!) : null);
		}
		catch (Exception e) when (e is IOException or InvalidOperationException or ArgumentException or NotSupportedException or
		                              SharpCompressException or UnauthorizedAccessException or IndexOutOfRangeException or InvalidDataException)
		{
			return new Contents(false, null); // Not an archive, damaged, encrypted headers or still being copied.
		}
	}

	private static string Folder(string name) => name.Contains('/') ? name[..name.LastIndexOf('/')] : "";

	private static string? CacheInfo(string archive, List<FileInfo> parts, IArchive opened, string root)
	{
		var key   = Convert.ToHexString(System.Security.Cryptography.SHA1.HashData(System.Text.Encoding.UTF8.GetBytes(
		    archive.ToLowerInvariant() + "|" + parts.Sum(p => p.Length))))[..16];
		var app    = Path.Combine(AppContext.BaseDirectory, "_Cache", "archive", key, "app0");
		var sceSys = Path.Combine(app, "sce_sys");
		if (File.Exists(Path.Combine(sceSys, "param.json"))) return app;
		var prefix = root.Length == 0 ? "sce_sys/" : root + "/sce_sys/";
		string[] keep = { "param.json", "icon0.png", "pic0.png", "pic1.png", "snd0.at9" };
		Directory.CreateDirectory(sceSys);
		foreach (var entry in opened.Entries)
		{
			if (entry.IsDirectory || entry.IsEncrypted || entry.Key is not { } name) continue;
			name = name.Replace('\\', '/');
			if (!name.StartsWith(prefix, StringComparison.OrdinalIgnoreCase) || !keep.Contains(name[prefix.Length..], StringComparer.OrdinalIgnoreCase)) continue;
			using var source = entry.OpenEntryStream();
			using var target = File.Create(Path.Combine(sceSys, name[prefix.Length..].ToLowerInvariant()));
			source.CopyTo(target);
		}
		return File.Exists(Path.Combine(sceSys, "param.json")) ? app : null;
	}

	public static async Task ExtractAsync(string archive, IProgress<Packages.InstallProgress> progress, CancellationToken cancel)
	{
		var output = OutputFor(archive);
		var parts  = Parts(archive);
		// An interrupted unpack of this same archive goes on where it stopped (the files already written whole are
		// kept); anything else left in a marked folder starts over.
		var marker = Path.Combine(output, Packages.Marker);
		if (File.Exists(marker) && File.ReadAllText(marker).Trim() != parts[0].FullName) CleanUp(archive);
		if (Directory.Exists(output) && !File.Exists(marker))
		{
			throw new IOException(L.F("Ya existe la carpeta {0}; muévela o bórrala para descomprimir este archivo", output));
		}
		progress.Report(new Packages.InstallProgress(File.Exists(marker) ? L.T("Retomando la descompresión…") : L.T("Leyendo el archivo…"), null));
		if (parts.Any(p => !p.Exists)) throw new IOException(L.T("Faltan partes del archivo: ") + string.Join(", ", parts.Where(p => !p.Exists).Select(p => p.Name)));

		try
		{
			await Task.Run(() => Unpack(parts, output, progress, cancel), cancel);
		}
		catch (Exception e) when (e is SharpCompressException or IndexOutOfRangeException)
		{
			throw new InvalidDataException(L.T("El archivo está dañado o incompleto: ") + e.Message);
		}

		// A game folder or a .pkg (installed next); nothing else is worth keeping.
		var holdsGame = Directory.EnumerateFiles(output, "eboot.bin", SearchOption.AllDirectories).Any() ||
		                Directory.EnumerateFiles(output, "*.pkg", SearchOption.AllDirectories).Any();
		if (!holdsGame) throw new InvalidDataException("El archivo no contiene un juego de PS5 (falta eboot.bin) ni un .pkg");
		File.Delete(Path.Combine(output, Packages.Marker));
		progress.Report(new Packages.InstallProgress(L.T("Descomprimido"), 1));
	}

	private static void Unpack(List<FileInfo> parts, string output, IProgress<Packages.InstallProgress> progress, CancellationToken cancel)
	{
		using var opened = OpenFor(parts);
		var files = opened.Entries.Where(e => !e.IsDirectory).ToList();
		if (opened.IsEncrypted || files.Any(e => e.IsEncrypted))
		{
			throw new InvalidDataException(L.T("El archivo tiene contraseña; descomprímelo tú con ella en tu carpeta de juegos"));
		}
		var root  = Path.GetFullPath(output) + Path.DirectorySeparatorChar;
		string Target(IEntry entry) => Path.GetFullPath(Path.Combine(output, (entry.Key ?? "").Replace('/', Path.DirectorySeparatorChar)));
		bool Written(IEntry entry, string target) => File.Exists(target) && new FileInfo(target).Length == entry.Size;
		var total = files.Sum(e => e.Size);
		var kept  = files.Where(e => Written(e, Target(e))).Sum(e => e.Size); // From an interrupted unpack.
		var free  = new DriveInfo(Path.GetPathRoot(Path.GetFullPath(output))!).AvailableFreeSpace;
		if (free < total - kept + (2L << 30))
		{
			throw new IOException(L.F("No hay espacio: el juego ocupa {0:0} GB y hacen falta {1:0} GB libres", total / (double)(1L << 30), (total - kept) / (double)(1L << 30) + 2));
		}

		Directory.CreateDirectory(output);
		File.WriteAllText(Path.Combine(output, Packages.Marker), parts[0].FullName);
		var buffer = new byte[4 << 20];
		var clock  = Stopwatch.StartNew();
		long done  = 0, filesDone = 0, lastDone = 0;
		double speed = 0, lastReport = 0;
		void Write(IEntry entry, Func<Stream> open)
		{
			cancel.ThrowIfCancellationRequested();
			var target = Target(entry);
			if (!target.StartsWith(root, StringComparison.OrdinalIgnoreCase)) return; // Paths that leave the folder ("..\").
			if (entry.IsDirectory)
			{
				Directory.CreateDirectory(target);
				return;
			}
			if (Written(entry, target)) // Already unpacked whole before an interruption.
			{
				done += entry.Size;
				filesDone++;
				return;
			}
			Directory.CreateDirectory(Path.GetDirectoryName(target)!);
			using (var source = open())
			using (var file = new FileStream(target, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20))
			{
				int read;
				while ((read = source.Read(buffer, 0, buffer.Length)) > 0)
				{
					file.Write(buffer, 0, read);
					done += read;
					var now = clock.Elapsed.TotalSeconds;
					if (now - lastReport < 0.5) continue;
					var rate = (done - lastDone) / (now - lastReport);
					speed      = speed == 0 ? rate : speed * 0.7 + rate * 0.3;
					lastDone   = done;
					lastReport = now;
					progress.Report(Progress(done, total, speed, filesDone, files.Count));
				}
			}
			filesDone++;
		}

		// Solid archives (and 7z) unpack in one pass from the start; the others entry by entry.
		if (opened.IsSolid || opened.Type == ArchiveType.SevenZip)
		{
			using var reader = opened.ExtractAllEntries();
			while (reader.MoveToNextEntry()) Write(reader.Entry, reader.OpenEntryStream);
		}
		else
		{
			foreach (var entry in opened.Entries) Write(entry, entry.OpenEntryStream);
		}
		progress.Report(new Packages.InstallProgress(L.T("Terminando…"), null));
	}

	private static IArchive OpenFor(List<FileInfo> parts)
	{
		try
		{
			return ArchiveFactory.OpenArchive(parts, new ReaderOptions());
		}
		catch (Exception e) when (e is SharpCompressException or InvalidOperationException or ArgumentException or NotSupportedException)
		{
			throw new InvalidDataException(L.T("No se pudo abrir el archivo (dañado o incompleto): ") + e.Message);
		}
	}

	private static Packages.InstallProgress Progress(long done, long total, double speed, long filesDone, long filesTotal)
	{
		var remaining = speed > 0 && total > done ? TimeSpan.FromSeconds((total - done) / speed) : (TimeSpan?)null;
		return new Packages.InstallProgress("Descomprimiendo", total > 0 ? Math.Min(0.99, done / (double)total) : null,
		                                    done, total, speed, remaining, filesDone, filesTotal);
	}

	[GeneratedRegex(@"\.part0*(\d+)\.rar$", RegexOptions.IgnoreCase)]
	private static partial Regex PartNumber();
}
