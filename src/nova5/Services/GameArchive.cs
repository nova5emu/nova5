using System.Diagnostics;

namespace Nova5.Services;

// Packs an installed game folder into one compressed .zar (ZArchive) with the bundled tools\zarchive.exe; the
// emulator plays it without unpacking (--game <file.zar>). Beside it, "<file>.zar.meta\sce_sys" keeps the game's
// sce_sys so the library shows its art, music, patches and trophies. The game folder is left untouched.
public static class GameArchive
{
	public static string Tool => Path.Combine(AppContext.BaseDirectory, "tools", "zarchive.exe");

	public static string Meta(string archive) => archive + ".meta";

	// A .zar without its "<file>.zar.meta" (packed by hand with zarchive.exe, or the folder lost): its sce_sys is taken out
	// of the archive again (a few MB, a second or two), so the library shows the game. False when that fails.
	public static bool RestoreMeta(string archive)
	{
		if (File.Exists(Path.Combine(Meta(archive), "sce_sys", "param.json"))) return true;
		if (!File.Exists(Tool)) return false;
		try
		{
			var info = new ProcessStartInfo(Tool) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
			info.ArgumentList.Add(archive);
			info.ArgumentList.Add(Meta(archive));
			info.ArgumentList.Add("/sce_sys");
			using var process = Process.Start(info);
			if (process is null) return false;
			process.StandardOutput.ReadToEnd();
			if (!process.WaitForExit(120_000)) process.Kill(entireProcessTree: true);
		}
		catch (Exception e) when (e is IOException or System.ComponentModel.Win32Exception or InvalidOperationException)
		{
			return false;
		}
		return File.Exists(Path.Combine(Meta(archive), "sce_sys", "param.json"));
	}

	public static string OutputFor(Game game) =>
	    Path.Combine(Path.GetDirectoryName(Path.GetFullPath(game.Dir).TrimEnd(Path.DirectorySeparatorChar))!, $"{game.TitleId}-{game.Version}.zar");

	// The .zar packed from this game folder, once it is complete.
	public static string? Packed(Game game) =>
	    game.Archive is null && File.Exists(OutputFor(game)) && File.Exists(Path.Combine(Meta(OutputFor(game)), "sce_sys", "param.json"))
	        ? OutputFor(game) : null;

	public static long FolderSize(string dir) =>
	    new DirectoryInfo(dir).EnumerateFiles("*", SearchOption.AllDirectories).Sum(f => f.Length);

	// Progress is the share of the folder's bytes already added (the tool prints "Adding <path>" per file).
	public static async Task<string> PackAsync(Game game, IProgress<double> progress, CancellationToken cancel)
	{
		if (!File.Exists(Tool)) throw new FileNotFoundException(L.T("Falta el compresor (tools\\zarchive.exe)"));
		var output = OutputFor(game);
		if (File.Exists(output)) throw new IOException(L.F("Ya existe {0}", output));
		var root  = Path.GetFullPath(game.Dir).TrimEnd(Path.DirectorySeparatorChar);
		var sizes = new DirectoryInfo(root).EnumerateFiles("*", SearchOption.AllDirectories)
		    .ToDictionary(f => Path.GetRelativePath(root, f.FullName).Replace('\\', '/'), f => f.Length, StringComparer.OrdinalIgnoreCase);
		var total = Math.Max(1, sizes.Values.Sum());
		var free  = new DriveInfo(Path.GetPathRoot(output)!).AvailableFreeSpace;
		if (free < total / 2) throw new IOException(L.F("No hay espacio: hacen falta unos {0:0} GB libres", total / 2e9));

		var info = new ProcessStartInfo(Tool) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
		info.ArgumentList.Add(root);
		info.ArgumentList.Add(output);
		using var process = Process.Start(info) ?? throw new IOException(L.T("No se pudo iniciar el compresor"));
		var errors = process.StandardError.ReadToEndAsync(cancel);
		long done = 0;
		var problems = new List<string>(); // The tool reports a file it cannot read on stdout.
		try
		{
			while (await process.StandardOutput.ReadLineAsync(cancel) is { } line)
			{
				if (!line.StartsWith("Adding ", StringComparison.Ordinal))
				{
					if (line.Length > 0 && line != "Archive complete") problems.Add(line);
					continue;
				}
				var path = line[7..].Trim().TrimStart('/');
				var rel  = path.StartsWith(root.Replace('\\', '/'), StringComparison.OrdinalIgnoreCase) ? path[root.Length..].TrimStart('/') : path;
				done += sizes.GetValueOrDefault(rel);
				progress.Report(Math.Min(0.99, (double)done / total));
			}
			await process.WaitForExitAsync(cancel);
		}
		catch (OperationCanceledException)
		{
			process.Kill(entireProcessTree: true);
			TryDelete(output);
			throw;
		}
		if (process.ExitCode != 0 || !File.Exists(output) || new FileInfo(output).Length == 0)
		{
			TryDelete(output);
			var reason = problems.LastOrDefault() ?? (await errors).Trim();
			throw new IOException(reason.StartsWith("Failed to open input file ") ? L.T("no se pudo leer ") + reason["Failed to open input file ".Length..]
			    : reason.Length > 0 ? reason : L.F("el compresor terminó con el código {0}", process.ExitCode));
		}

		// The game's sce_sys beside the archive, for the library (title, art, music, trophies, patches).
		var meta = Path.Combine(Meta(output), "sce_sys");
		foreach (var file in Directory.EnumerateFiles(Path.Combine(root, "sce_sys"), "*", SearchOption.AllDirectories))
		{
			var target = Path.Combine(meta, Path.GetRelativePath(Path.Combine(root, "sce_sys"), file));
			Directory.CreateDirectory(Path.GetDirectoryName(target)!);
			File.Copy(file, target, overwrite: true);
		}
		progress.Report(1);
		return output;
	}

	private static void TryDelete(string file)
	{
		try
		{
			if (File.Exists(file)) File.Delete(file);
		}
		catch (IOException)
		{
		}
	}

	// The original folder to the Recycle Bin (Windows may ask to delete it for good when it is too big for it).
	public static void RecycleFolder(string dir) =>
	    Microsoft.VisualBasic.FileIO.FileSystem.DeleteDirectory(dir, Microsoft.VisualBasic.FileIO.UIOption.OnlyErrorDialogs,
	        Microsoft.VisualBasic.FileIO.RecycleOption.SendToRecycleBin);
}
