using System.Diagnostics;
using System.Text.RegularExpressions;

namespace Nova5.Services;

// Path is the file the engine boots: a .nsp/.xci, or the "main" of an extracted game (decrypted: no keys needed).
public sealed record SwitchGame(string Name, string Path, long Size)
{
	public bool    Extracted { get; init; }
	public string? Icon      { get; init; } // icon_*.dat (a JPEG) of an extracted game, when it has one.
	// A loose dump (base\, updates\<version>\, dlc\<id>\, as console dump tools write it): its folder. Path boots its base;
	// packing it into a .nup (PackNup) brings its update and DLC along.
	public string? DumpRoot  { get; init; }
	public bool    HasExtras { get; init; } // The dump has updates or DLC.
	public bool    NamedFromFolder { get; init; } // No title in the game's own data: the name comes from its folder.
}

// NovaS: the Switch 2 side of Nova5. Its engine is Eglantine (GPL-3.0, derived from Eden, yuzu and Sudachi), kept
// portable in "novas\" next to the launcher with its own "user\" folder for keys, firmware, saves and settings.
public static partial class SwitchGames
{
	private static readonly string[] Extensions = { ".nsp", ".xci", ".nsz", ".xcz", ".nup" };

	public static string EngineDir => System.IO.Path.Combine(AppContext.BaseDirectory, "novas");
	public static string Engine    => System.IO.Path.Combine(EngineDir, "NovaSEmulator.exe");
	public static string UserDir   => System.IO.Path.Combine(EngineDir, "user");
	public static string KeysDir   => System.IO.Path.Combine(UserDir, "keys");
	public static bool   HasEngine => File.Exists(Engine);
	public static bool   HasKeys   => File.Exists(System.IO.Path.Combine(KeysDir, "prod.keys"));

	// Games (.nsp, .xci and their compressed forms, .nup and extracted games) up to five levels below each folder: a loose
	// dump's exefs sits at <game>\<dump>\base\exefs.
	public static List<SwitchGame> Scan(IEnumerable<string> roots)
	{
		var found = new Dictionary<string, SwitchGame>(StringComparer.OrdinalIgnoreCase);
		foreach (var root in roots.Where(Directory.Exists))
		{
			foreach (var dir in Folders(root, 5))
			{
				try
				{
					if (ReadExtracted(dir) is { } extracted) found.TryAdd(System.IO.Path.GetFullPath(extracted.Path), extracted);
					foreach (var file in Directory.EnumerateFiles(dir))
					{
						if (!Extensions.Contains(System.IO.Path.GetExtension(file).ToLowerInvariant())) continue;
						var game = new SwitchGame(CleanName(file), file, new FileInfo(file).Length);
						if (file.EndsWith(".nup", StringComparison.OrdinalIgnoreCase)) game = ReadNup(game);
						found.TryAdd(System.IO.Path.GetFullPath(file), game);
					}
				}
				catch (Exception e) when (e is UnauthorizedAccessException or IOException)
				{
				}
			}
		}
		// A loose dump already packed into its .nup shows once, as the .nup.
		var games = found.Values.Where(g => g.DumpRoot is null || !File.Exists(NupOf(g.DumpRoot))).Select(Dress);
		return games.OrderBy(g => g.Name, StringComparer.CurrentCultureIgnoreCase).ToList();
	}

	// The cover: the one the player chose, else NovaS's own for a known game, else the game's icon. A known game named
	// only from its folder also gets its proper name.
	private static SwitchGame Dress(SwitchGame game)
	{
		var known = SwitchCovers.Find(game.Name);
		if (known is { } k && game.NamedFromFolder) game = game with { Name = k.Name };
		if (CoverOf(game) is { } chosen) return game with { Icon = chosen };
		return known is { } match ? game with { Icon = match.Cover } : game;
	}

	// ---- Covers chosen by the player (_Cache\novas-covers, one per game file) ----

	private static string CoverBase(SwitchGame game) => System.IO.Path.Combine(AppContext.BaseDirectory, "_Cache", "novas-covers",
	    Convert.ToHexString(System.Security.Cryptography.SHA1.HashData(System.Text.Encoding.UTF8.GetBytes(game.Path.ToLowerInvariant())))[..16]);

	private static string? CoverOf(SwitchGame game) =>
	    new[] { ".png", ".jpg", ".jpeg", ".webp", ".bmp" }.Select(e => CoverBase(game) + e).FirstOrDefault(File.Exists);

	public static void SetCover(SwitchGame game, string image)
	{
		if (CoverOf(game) is { } old) File.Delete(old);
		Directory.CreateDirectory(System.IO.Path.GetDirectoryName(CoverBase(game))!);
		File.Copy(image, CoverBase(game) + System.IO.Path.GetExtension(image).ToLowerInvariant(), overwrite: true);
	}

	// The size of an extracted game, counted once per folder: Donkey Kong Bananza's dump alone has ~160,000 files, and
	// adding them up again on every scan (each return from a game) froze the launcher for seconds.
	// Kept in _Cache\novas-sizes.json between sessions, with the folder's own write time: a dump whose top folder changed
	// (an update or DLC added or removed) is counted again.
	private sealed record SizeEntry(long Size, long Stamp);

	private static readonly string SizesFile = System.IO.Path.Combine(AppContext.BaseDirectory, "_Cache", "novas-sizes.json");
	private static readonly Lazy<System.Collections.Concurrent.ConcurrentDictionary<string, SizeEntry>> FolderSizes = new(LoadSizes);

	private static System.Collections.Concurrent.ConcurrentDictionary<string, SizeEntry> LoadSizes()
	{
		try
		{
			if (File.Exists(SizesFile) &&
			    System.Text.Json.JsonSerializer.Deserialize<Dictionary<string, SizeEntry>>(File.ReadAllText(SizesFile)) is { } saved)
				return new(saved, StringComparer.OrdinalIgnoreCase);
		}
		catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Text.Json.JsonException)
		{
		}
		return new(StringComparer.OrdinalIgnoreCase);
	}

	private static long FolderSize(string folder)
	{
		var path = System.IO.Path.GetFullPath(folder);
		long stamp;
		try
		{
			stamp = Directory.GetLastWriteTimeUtc(path).Ticks;
		}
		catch (Exception e) when (e is IOException or UnauthorizedAccessException)
		{
			return 0;
		}
		if (FolderSizes.Value.TryGetValue(path, out var known) && known.Stamp == stamp) return known.Size;
		long size;
		try
		{
			size = Directory.EnumerateFiles(path, "*", SearchOption.AllDirectories).Sum(f => new FileInfo(f).Length);
		}
		catch (Exception e) when (e is UnauthorizedAccessException or IOException)
		{
			return 0;
		}
		FolderSizes.Value[path] = new SizeEntry(size, stamp);
		try
		{
			Directory.CreateDirectory(System.IO.Path.GetDirectoryName(SizesFile)!);
			File.WriteAllText(SizesFile, System.Text.Json.JsonSerializer.Serialize(FolderSizes.Value));
		}
		catch (Exception e) when (e is IOException or UnauthorizedAccessException)
		{
		}
		return size;
	}

	private static IEnumerable<string> Folders(string dir, int depth)
	{
		yield return dir;
		if (depth == 0) yield break;
		string[] children;
		try
		{
			children = Directory.GetDirectories(dir);
		}
		catch (Exception e) when (e is UnauthorizedAccessException or IOException)
		{
			yield break;
		}
		foreach (var child in children)
		{
			var name = System.IO.Path.GetFileName(child);
			if (name is "sce_sys" or "sce_module" or "app0" or "user" or "novas" or "romfs") continue; // PS5 games, the engine, game data.
			foreach (var nested in Folders(child, depth - 1)) yield return nested;
		}
	}

	// An extracted (decrypted) game: a folder with "main" and "main.npdm" (its ExeFS), usually "exefs" next to "romfs".
	// Its name and icon come from control.nacp and icon_*.dat when they were extracted too, else from the folder name.
	private static SwitchGame? ReadExtracted(string exefs)
	{
		var main = System.IO.Path.Combine(exefs, "main");
		if (!File.Exists(main) || !File.Exists(System.IO.Path.Combine(exefs, "main.npdm"))) return null;
		var game = System.IO.Path.GetFileName(exefs).Equals("exefs", StringComparison.OrdinalIgnoreCase)
		    ? System.IO.Path.GetDirectoryName(exefs)! : exefs;
		// A loose dump: its base is the game; an update's or a DLC's exefs is not a game of its own.
		var parent = System.IO.Path.GetDirectoryName(game);
		var parentName = System.IO.Path.GetFileName(parent ?? "");
		if (parentName.Equals("updates", StringComparison.OrdinalIgnoreCase) || parentName.Equals("dlc", StringComparison.OrdinalIgnoreCase)) return null;
		string? dumpRoot = null;
		if (System.IO.Path.GetFileName(game).Equals("base", StringComparison.OrdinalIgnoreCase) && parent is not null) dumpRoot = parent;
		var places = new[] { game, exefs, System.IO.Path.Combine(game, "control") }.Where(Directory.Exists).ToList();
		var nacp   = places.Select(p => System.IO.Path.Combine(p, "control.nacp")).FirstOrDefault(File.Exists);
		var icon   = places.SelectMany(p => Directory.EnumerateFiles(p, "icon_*.dat"))
		    .OrderBy(f => f.Contains("LatinAmericanSpanish", StringComparison.OrdinalIgnoreCase) ? 0 : f.Contains("AmericanEnglish", StringComparison.OrdinalIgnoreCase) ? 1 : 2)
		    .FirstOrDefault();
		var title = nacp is null ? null : TitleFromNacp(nacp);
		var folder = dumpRoot ?? game;
		var size = FolderSize(folder);
		var extras = dumpRoot is not null && new[] { "updates", "dlc" }.Any(d =>
		    Directory.Exists(System.IO.Path.Combine(dumpRoot, d)) && Directory.EnumerateDirectories(System.IO.Path.Combine(dumpRoot, d)).Any());
		return new SwitchGame(title ?? Words(Clean(System.IO.Path.GetFileName(folder))), main, size)
		{
			Extracted = true, Icon = icon, DumpRoot = dumpRoot, HasExtras = extras, NamedFromFolder = title is null,
		};
	}

	// A .nup (Eglantine's "NX Package"): loose, already decrypted ExeFS/RomFS of a game, its updates and DLC in one file,
	// so it needs no keys. Its base package's meta carries the title and icon (see nup_container.h in the engine):
	// superblock "NUP\x01" with the TOC offset at 0x08; TOC header 0x30 bytes, then 0x40-byte package records (kind at
	// 0x18, meta offset/size at 0x20/0x28); meta is TLVs (u16 type, u16, u32 size): 3 icon, 5 title, 9 localized titles.
	private static SwitchGame ReadNup(SwitchGame game)
	{
		game = game with { Extracted = true };
		try
		{
			using var file = File.OpenRead(game.Path);
			using var read = new BinaryReader(file);
			if (read.ReadUInt32() != 0x0150554E) return game; // "NUP\x01"
			file.Position = 0x08;
			var toc = read.ReadInt64();
			file.Position = toc + 4;
			var packages = read.ReadUInt32();
			long metaAt = 0, metaSize = 0;
			for (var i = 0u; i < Math.Min(packages, 64u); i++)
			{
				file.Position = toc + 0x30 + i * 0x40L;
				var record = read.ReadBytes(0x40);
				var at = BitConverter.ToInt64(record, 0x20);
				var size = BitConverter.ToUInt32(record, 0x28);
				if (at == 0 || size == 0 || size > 64 << 20) continue;
				if (metaAt == 0 || record[0x18] == 0) (metaAt, metaSize) = (at, size); // The base program first.
				if (record[0x18] == 0) break;
			}
			if (metaAt == 0) return game;
			file.Position = metaAt;
			var meta = read.ReadBytes((int)metaSize);
			string? title = null, spanish = null;
			byte[]? icon = null;
			for (var pos = 0; pos + 8 <= meta.Length;)
			{
				var type = BitConverter.ToUInt16(meta, pos);
				var size = (int)BitConverter.ToUInt32(meta, pos + 4);
				pos += 8;
				if (size < 0 || pos + size > meta.Length) break;
				if (type == 5) title = System.Text.Encoding.UTF8.GetString(meta, pos, size).TrimEnd('\0');
				else if (type == 3) icon = meta[pos..(pos + size)];
				else if (type == 9) spanish = Localized(meta[pos..(pos + size)]);
				pos += size;
			}
			var name = spanish ?? title;
			string? iconPath = null;
			if (icon is { Length: > 0 })
			{
				var cache = System.IO.Path.Combine(AppContext.BaseDirectory, "_Cache", "nup");
				Directory.CreateDirectory(cache);
				iconPath = System.IO.Path.Combine(cache, Convert.ToHexString(System.Security.Cryptography.SHA1.HashData(icon))[..16] + ".jpg");
				if (!File.Exists(iconPath)) File.WriteAllBytes(iconPath, icon);
			}
			return game with { Name = string.IsNullOrWhiteSpace(name) ? game.Name : name.Trim(), Icon = iconPath };
		}
		catch (Exception e) when (e is IOException or UnauthorizedAccessException or ArgumentException or EndOfStreamException)
		{
			return game;
		}
	}

	// Localized titles: records of (u8 language length, language, u32 length, title); a Spanish one if there is.
	private static string? Localized(byte[] data)
	{
		for (var pos = 0; pos < data.Length;)
		{
			int length = data[pos++];
			if (pos + length + 4 > data.Length) break;
			var language = System.Text.Encoding.ASCII.GetString(data, pos, length);
			pos += length;
			var size = (int)BitConverter.ToUInt32(data, pos);
			pos += 4;
			if (size < 0 || pos + size > data.Length) break;
			var title = System.Text.Encoding.UTF8.GetString(data, pos, size).TrimEnd('\0');
			pos += size;
			if (title.Length > 0 && (language.StartsWith("es", StringComparison.OrdinalIgnoreCase) || language.Contains("Spanish", StringComparison.OrdinalIgnoreCase)))
				return title;
		}
		return null;
	}

	// control.nacp starts with 16 titles of 0x300 bytes (name 0x200, publisher 0x100), one per language; Latin American
	// Spanish is 5, Spanish 6, American English 0.
	private static string? TitleFromNacp(string nacp)
	{
		try
		{
			var data = File.ReadAllBytes(nacp);
			foreach (var language in new[] { 5, 6, 0, 1 })
			{
				var at = language * 0x300;
				if (at + 0x200 > data.Length) continue;
				var name = System.Text.Encoding.UTF8.GetString(data, at, 0x200).TrimEnd('\0').Trim();
				if (name.Length > 0) return name;
			}
		}
		catch (IOException)
		{
		}
		return null;
	}

	// "DonkeyKongBananza" -> "Donkey Kong Bananza" (folder names written without spaces).
	private static string Words(string name) =>
	    name.Contains(' ') ? name : System.Text.RegularExpressions.Regex.Replace(name, "(?<=[a-z0-9])(?=[A-Z])", " ");

	// ---- Loose dumps packed into a .nup: the base, its updates and DLC in one file that needs no keys ----

	// Where the engine writes it: next to the dump folder, with the folder's name.
	public static string NupOf(string dumpRoot) => System.IO.Path.TrimEndingDirectorySeparator(dumpRoot) + ".nup";

	// The engine packs the dump (eden-cli --pack-nup); its progress lines ("12 / 3400 MiB, ...") come on stderr.
	public static Process PackNup(string dumpRoot)
	{
		var info = new ProcessStartInfo(Engine)
		{
			UseShellExecute = false, WorkingDirectory = EngineDir, CreateNoWindow = true, RedirectStandardError = true, RedirectStandardOutput = true,
		};
		info.ArgumentList.Add("--pack-nup");
		info.ArgumentList.Add(dumpRoot);
		return Process.Start(info) ?? throw new InvalidOperationException(L.T("No se pudo iniciar el empaquetado"));
	}

	// "Mario Kart World [0100...][v0] (NSP).nsp" -> "Mario Kart World".
	public static string CleanName(string file) => Clean(System.IO.Path.GetFileNameWithoutExtension(file));

	private static string Clean(string raw)
	{
		var name = Brackets().Replace(raw, " ");
		name = name.Replace('_', ' ').Replace('.', ' ');
		name = Spaces().Replace(name, " ").Trim(' ', '-');
		return name.Length > 0 ? name : raw;
	}

	// The game full screen, in NovaSEmulator (the engine without any window of its own around the game).
	public static Process Start(SwitchGame game)
	{
		Directory.CreateDirectory(KeysDir); // Portable mode: everything stays under novas\user.
		ApplyDefaults();
		var info = new ProcessStartInfo(Engine) { UseShellExecute = false, WorkingDirectory = EngineDir };
		// The engine's Switch 2 tuning: GPU method work on its own thread (up to +54 % FPS) and batched fence syncs.
		// Narrower waits between the two GPU threads (Lichii #179/#231): semaphores and command lists without draining.
		var tuning = new List<(string, string)> { ("EDEN_NX2_SPLIT", "2"), ("EDEN_NX2_FENCE_BATCH", "1"),
			("EDEN_NX2_SEM_FAST", "2"), ("EDEN_NX2_LIST_FAST", "1"), ("EDEN_NX2_LOOP_SKIP", "1") };
		tuning.AddRange(Options.Where(o => o.Section == EnvSection).Select(o => (o.Key, o.Choices[Current(o)].Value)));
		foreach (var (name, value) in tuning)
			if (Environment.GetEnvironmentVariable(name) is null) info.Environment[name] = value;
		// The engine hides its window while its disk shaders build and writes the progress here for the launcher.
		Directory.CreateDirectory(System.IO.Path.GetDirectoryName(LoadingFile)!);
		File.Delete(LoadingFile);
		info.Environment["NOVAS_LOADING_FILE"] = LoadingFile;
		var fullscreen = Options.First(o => o.Section == LaunchSection && o.Key == "fullscreen");
		if (fullscreen.Choices[Current(fullscreen)].Value == "true") info.ArgumentList.Add("-f");
		info.ArgumentList.Add("-g");
		info.ArgumentList.Add(game.Path);
		return Process.Start(info) ?? throw new InvalidOperationException(L.T("No se pudo iniciar NovaS"));
	}

	// Where the engine writes how far its disk shaders have built ("<value> <total>", then "done"), see LaunchSwitch.
	public static string LoadingFile => System.IO.Path.Combine(UserDir, "log", "novas-loading.txt");

	// The games' saves (user/nand/user/save): one folder per user and game, to copy or bring saves from elsewhere.
	public static string SavesDir => System.IO.Path.Combine(UserDir, "nand", "user", "save");

	public static void OpenSavesFolder()
	{
		Directory.CreateDirectory(SavesDir);
		Process.Start(new ProcessStartInfo(SavesDir) { UseShellExecute = true });
	}

	public static string FirmwareDir => System.IO.Path.Combine(UserDir, "nand", "system", "Contents", "registered");
	public static bool   HasFirmware => Directory.Exists(FirmwareDir) && Directory.EnumerateFiles(FirmwareDir, "*.nca").Any();

	// The console's keys (prod.keys, title.keys...) copied into user\keys, where the engine reads them.
	public static int InstallKeys(IEnumerable<string> files)
	{
		Directory.CreateDirectory(KeysDir);
		var copied = 0;
		foreach (var file in files.Where(f => f.EndsWith(".keys", StringComparison.OrdinalIgnoreCase)))
		{
			File.Copy(file, System.IO.Path.Combine(KeysDir, System.IO.Path.GetFileName(file).ToLowerInvariant()), overwrite: true);
			copied++;
		}
		return copied;
	}

	// The console's firmware, as the engine's own installer does it: every .nca of a folder (or of a .zip, at any depth)
	// into nand\system\Contents\registered, emptied first. Returns how many were installed.
	public static int InstallFirmware(string source, IProgress<double> progress)
	{
		var zip = source.EndsWith(".zip", StringComparison.OrdinalIgnoreCase);
		using var archive = zip ? System.IO.Compression.ZipFile.OpenRead(source) : null;
		var entries = archive?.Entries.Where(e => e.Name.EndsWith(".nca", StringComparison.OrdinalIgnoreCase)).ToList();
		var files   = zip ? new List<string>() : Directory.GetFiles(source, "*.nca").ToList();
		var count   = entries?.Count ?? files.Count;
		if (count == 0) throw new InvalidDataException(L.T("No hay archivos .nca de firmware ahí"));
		if (Directory.Exists(FirmwareDir)) Directory.Delete(FirmwareDir, recursive: true);
		Directory.CreateDirectory(FirmwareDir);
		for (var i = 0; i < count; i++)
		{
			if (entries is not null) System.IO.Compression.ZipFileExtensions.ExtractToFile(entries[i], System.IO.Path.Combine(FirmwareDir, entries[i].Name), overwrite: true);
			else File.Copy(files[i], System.IO.Path.Combine(FirmwareDir, System.IO.Path.GetFileName(files[i])), overwrite: true);
			progress.Report((i + 1) / (double)count);
		}
		return count;
	}

	// ---- Settings: NovaSEmulator reads user\config\sdl2-config.ini; each value goes with its "\default=false" mark. ----

	// Options in this section are environment switches for the engine, set when a game starts (not ini keys).
	public const string EnvSection = "Entorno";

	// Options in this section are how the launcher starts a game (kept in the choices file, not the engine's ini).
	public const string LaunchSection = "Lanzador";

	public sealed record Option(string Label, string Detail, string Section, string Key, (string Value, string Name)[] Choices);

	public static readonly Option[] Options =
	{
		// The engine's scale over the game's own resolution (1440p in Dock): 3 = 1x, 2 = 3/4 (1080p), 1 = 1/2 (720p).
		new(L.T("Resolución"), L.T("Más resolución se ve más nítido pero pide más a la tarjeta gráfica. En PC menos potentes, como la ROG Ally o un portátil, 1080p o 720p van más rápido y en una pantalla pequeña se ven bien."), "Renderer", "resolution_setup",
		    new[] { ("5", "4K (1.5x)"), ("3", L.T("Nativa (1440p)")), ("6", "2x (5K)"), ("7", "3x"), ("2", L.T("1080p (menos calidad)")), ("1", L.T("720p (la más ligera)")) }),
		new(L.T("Modo de consola"), L.T("Dock: como conectada a la tele (más resolución). Portátil: como en la mano."), "System", "use_docked_mode",
		    new[] { ("1", "Dock (TV)"), ("0", L.T("Portátil")) }),
		// The engine's AspectRatio enum: 0 = 16:9, 2 = 21:9, 3 = 16:10, 4 = stretch to the window.
		new(L.T("Pantalla"), L.T("16:9 es la imagen original. Estirar llena toda la ventana o el monitor (en pantallas ultrawide o 16:10 se ve sin franjas, ") +
		    L.T("aunque un poco ancha)."), "Renderer", "aspect_ratio",
		    new[] { ("0", L.T("16:9 (original)")), ("4", L.T("Estirar a la pantalla")), ("2", "21:9 (ultrawide)"), ("3", "16:10") }),
		new(L.T("Abrir los juegos"), L.T("Pantalla completa, o en una ventana que puedes mover y agrandar (F11 cambia entre las dos con el juego abierto)."), LaunchSection, "fullscreen",
		    new[] { ("true", L.T("Pantalla completa")), ("false", L.T("En ventana")) }),
		// NOVAS_BUTTON_LABELS (yuzu_cmd/auto_controller.cpp): swap A/B and X/Y of a detected pad.
		new(L.T("Botones del mando"), L.T("Según la letra: con un mando de Xbox o de PC, A es A y B es B, como en los juegos de PC. ") +
		    L.T("Posición de Switch: el botón de la derecha hace de A, como en la consola."), EnvSection, "NOVAS_BUTTON_LABELS",
		    new[] { ("0", L.T("Posición de Switch")), ("1", L.T("Según la letra (Xbox)")) }),
		new(L.T("Sincronización vertical"), L.T("Sí evita cortes en la imagen; No da la menor latencia."), "Renderer", "use_vsync",
		    new[] { ("2", L.T("Sí")), ("1", "Mailbox"), ("0", "No") }),
		new(L.T("Idioma de los juegos"), L.T("El idioma de la consola que ven los juegos."), "System", "language_index",
		    // Values: the engine's Language enum (common/settings_enums.h).
		    new[] { ("14", L.T("Español (Latinoamérica)")), ("5", L.T("Español (España)")), ("1", L.T("Inglés")), ("12", L.T("Inglés (Reino Unido)")),
		            ("17", L.T("Portugués (Brasil)")), ("9", L.T("Portugués (Portugal)")), ("2", L.T("Francés")), ("13", L.T("Francés (Canadá)")),
		            ("3", L.T("Alemán")), ("4", L.T("Italiano")), ("8", L.T("Neerlandés")), ("10", L.T("Ruso")), ("18", L.T("Polaco")),
		            ("0", L.T("Japonés")), ("7", L.T("Coreano")), ("15", L.T("Chino simplificado")), ("16", L.T("Chino tradicional")), ("19", L.T("Tailandés")) }),
		new(L.T("Límite de velocidad"), L.T("Sí: el juego va a su velocidad real. No: tan rápido como pueda el PC."), "Core", "use_speed_limit",
		    new[] { ("true", L.T("Sí")), ("false", "No") }),
		new(L.T("Caché de shaders"), L.T("Guarda los shaders compilados para que las siguientes partidas no den tirones."), "Renderer", "use_disk_shader_cache",
		    new[] { ("true", L.T("Sí")), ("false", "No") }),
		new(L.T("Precisión de la GPU"), L.T("Normal: reparte el trabajo de la GPU en dos hilos (en Zelda, hasta 9 FPS más que Alta). ") +
		    L.T("Alta: un solo hilo, por si algún juego se ve mal con Normal."), "Renderer", "gpu_accuracy",
		    new[] { ("0", L.T("Normal (rápida)")), ("1", L.T("Alta")) }),
		new(L.T("Lecturas rápidas de la GPU"), L.T("Copia antes de tiempo los datos que el juego lee de la tarjeta, así no la espera (en Zelda, de 44 a 52 FPS ") +
		    L.T("en el pueblo Kokiri). Si ves parpadeos, pon No."), EnvSection, "EDEN_NX2_PREFLUSH",
		    new[] { ("1", L.T("Sí (más FPS)")), ("0", "No") }),
		new("Zelda: Ocarina of Time — FPS", L.T("60 dinámico: hasta 60 FPS y el juego ajusta su tiempo, así nunca va más lento aunque bajen los FPS. ") +
		    L.T("30 estables: siempre 30 FPS a velocidad normal. 60 fijo: el original (va más lento si baja de 60)."), "Renderer", "oot_frame_mode",
		    new[] { ("2", L.T("60 dinámico")), ("1", L.T("30 estables")), ("0", L.T("60 fijo")) }),
		new(L.T("Zelda: antialiasing del juego"), L.T("Sin nitidez: se salta la red de antialiasing y nitidez del juego (unos 3-4 FPS más; la imagen ") +
		    L.T("queda un poco más suave). Original: como en la consola."), "Renderer", "oot_anti_aliasing",
		    new[] { ("1", L.T("Sin nitidez (más FPS)")), ("0", "Original") }),
		new(L.T("Generación de fotogramas (FSR3)"), L.T("Inventa un fotograma entre cada dos: un juego a 30 FPS se ve a 60. Con Zelda, lo más fluido es ") +
		    L.T("\"30 estables\" + esto. Puede dar un poco más de retraso al mando y algún borde raro en movimientos rápidos."), "Renderer", "frame_gen",
		    new[] { ("true", L.T("Sí (x2)")), ("false", "No") }),
		new(L.T("Compilación de shaders"), L.T("En segundo plano: los shaders nuevos se preparan sin frenar el juego (algún objeto puede tardar un instante en ") +
		    L.T("aparecer la primera vez). Esperando: nada falta, pero hay tirones mientras se compilan."), "Renderer", "use_asynchronous_shaders",
		    new[] { ("true", L.T("En segundo plano")), ("false", L.T("Esperando")) }),
	};

	public static string SettingsPath => System.IO.Path.Combine(UserDir, "config", "sdl2-config.ini");

	// The chosen value's index, or the first (the default) when it was never set.
	// The player's own choices, kept apart from the engine's ini so NovaS's defaults can change without overriding them.
	public static string ChoicesPath => System.IO.Path.Combine(UserDir, "config", "novas-choices.ini");

	// The player's choice, else NovaS's default (the first).
	public static int Current(Option option)
	{
		var value = new IniFile(ChoicesPath).Get(option.Section, option.Key);
		return Math.Max(0, Array.FindIndex(option.Choices, c => c.Value == value));
	}

	// Every option into the engine's ini: the player's choice or NovaS's default, marked "\default=false" (the engine
	// ignores a value whose "\default" is true and writes its own back on exit).
	public static void ApplyDefaults()
	{
		Directory.CreateDirectory(System.IO.Path.GetDirectoryName(SettingsPath)!);
		var ini = new IniFile(SettingsPath);
		foreach (var option in Options.Where(o => o.Section != EnvSection && o.Section != LaunchSection))
		{
			ini.Set(option.Section, option.Key, option.Choices[Current(option)].Value);
			ini.Set(option.Section, option.Key + "\\default", "false");
		}
		ini.Save();
	}

	public static void Set(Option option, int choice)
	{
		Directory.CreateDirectory(System.IO.Path.GetDirectoryName(ChoicesPath)!);
		var choices = new IniFile(ChoicesPath);
		choices.Set(option.Section, option.Key, option.Choices[choice].Value);
		choices.Save();
		ApplyDefaults();
	}

	public static void OpenKeysFolder()
	{
		Directory.CreateDirectory(KeysDir);
		Process.Start(new ProcessStartInfo(KeysDir) { UseShellExecute = true });
	}

	[GeneratedRegex(@"\[[^\]]*\]|\([^)]*\)")]
	private static partial Regex Brackets();

	[GeneratedRegex(@"\s{2,}")]
	private static partial Regex Spaces();
}
