using System.Text.Json;
using System.Text.RegularExpressions;

namespace Nova5.Services;

public sealed record Game(string TitleId, string Name, string Version, string Dir)
{
	// Set for a .pkg not installed yet, or a .rar/.zip/.7z not unpacked yet (Archives.IsArchive); Dir then holds its
	// sce_sys read ahead for its art.
	public string? Package { get; init; }

	// Additional content (DLC): its param.json has no applicationCategoryType and no eboot.bin; TitleId is the game's.
	public bool    IsAddon   { get; init; }
	public string  ContentId { get; init; } = "";

	// A game packed into a .zar: the archive the emulator loads; Dir is then its "<file>.zar.meta" folder.
	public string? Archive   { get; init; }

	public string  Eboot      => Path.Combine(Dir, "eboot.bin");
	public string  SceSys     => Path.Combine(Dir, "sce_sys");
	public string? Icon       => FirstExisting("icon0.png");
	public string? Background => FirstExisting("pic0.png", "pic1.png", "pic2.png");
	public string? Music      => FirstExisting("snd0.at9");

	private string? FirstExisting(params string[] names) =>
	    names.Select(n => Path.Combine(SceSys, n)).FirstOrDefault(File.Exists);
}

// Finds installed PS5 games (folders with eboot.bin and sce_sys/param.json) and packages waiting to be
// installed (.pkg), up to three levels below each game folder.
public static class Library
{
	public static List<Game> Scan(IEnumerable<string> roots)
	{
		var games    = new Dictionary<string, Game>(StringComparer.OrdinalIgnoreCase);
		var packages = new List<string>();
		var archives = new List<string>();
		foreach (var root in roots.Where(Directory.Exists))
		{
			foreach (var dir in Candidates(root, 3))
			{
				if (Read(dir) is { } game) games.TryAdd(Path.GetFullPath(game.Dir), game);
				try
				{
					foreach (var archive in Directory.GetFiles(dir, "*.zar"))
						if (GameArchive.RestoreMeta(archive) && ReadArchive(archive) is { } packed)
							games.TryAdd(Path.GetFullPath(archive), packed);
				}
				catch (Exception e) when (e is UnauthorizedAccessException or IOException)
				{
				}
				try
				{
					packages.AddRange(Directory.GetFiles(dir, "*.pkg"));
					foreach (var pattern in Archives.Patterns)
						archives.AddRange(Directory.GetFiles(dir, pattern).Where(Archives.IsFirstVolume));
				}
				catch (Exception e) when (e is UnauthorizedAccessException or IOException)
				{
				}
			}
		}
		var installed = games.Values.Select(g => g.TitleId).ToHashSet(StringComparer.OrdinalIgnoreCase);
		var pending = new List<Game>();
		foreach (var package in packages.Distinct(StringComparer.OrdinalIgnoreCase))
		{
			if (Packages.IsInstalled(package)) continue;
			// The package's own param.json (read ahead into a cache) gives its title, so a game installed
			// anywhere else, such as an earlier manual extraction, is not installed twice.
			var info = Packages.ReadInfo(package) is { } dir ? Read(dir, requireEboot: false) : null;
			if (info is not null && !info.IsAddon && installed.Contains(info.TitleId)) continue;
			pending.Add(info is null
			    ? new Game("", PackageName(package), "", Path.GetDirectoryName(package)!) { Package = package }
			    : info with { Package = package, Name = info.IsAddon ? "DLC · " + info.Name : info.Name });
		}
		// Archives holding a game or a .pkg, not unpacked yet; their art when it can be read without unpacking.
		foreach (var archive in archives.Distinct(StringComparer.OrdinalIgnoreCase))
		{
			if (Archives.IsExtracted(archive) || !Archives.HoldsGame(archive)) continue;
			var info = Archives.ReadInfo(archive) is { } dir ? Read(dir, requireEboot: false) : null;
			if (info is not null && !info.IsAddon && installed.Contains(info.TitleId)) continue;
			pending.Add(info is null
			    ? new Game("", CleanName(Archives.BaseName(archive)), "", Path.GetDirectoryName(archive)!) { Package = archive }
			    : info with { Package = archive, Name = info.IsAddon ? "DLC · " + info.Name : info.Name });
		}
		return games.Values.OrderBy(g => g.Name, StringComparer.CurrentCultureIgnoreCase).Concat(pending).ToList();
	}

	private static string PackageName(string package) => CleanName(Path.GetFileNameWithoutExtension(package));

	private static string CleanName(string name)
	{
		name = Regex.Replace(name, @"^\[[^\]]*\]-?", "");                      // Release group tags such as [Group]-
		name = Regex.Replace(name, @"^[A-Z]{2}\d{4}-[A-Z]{4}\d{5}_\d{2}-", ""); // Content ID prefix
		return name.Replace('_', ' ').Replace('.', ' ').Trim();
	}

	private static IEnumerable<string> Candidates(string dir, int depth)
	{
		if (File.Exists(Path.Combine(dir, Packages.Marker))) yield break; // A package install in progress.
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
			var name = Path.GetFileName(child);
			if (name is "sce_sys" or "sce_module" or "data") continue;
			foreach (var nested in Candidates(child, depth - 1)) yield return nested;
		}
	}

	public static Game? ReadGame(string dir) => Read(dir);

	// A game packed into a .zar, from the sce_sys kept beside it.
	public static Game? ReadArchive(string archive) =>
	    Read(GameArchive.Meta(archive), requireEboot: false) is { IsAddon: false } packed ? packed with { Archive = archive } : null;

	public static Game? ReadAddon(string dir) => Read(dir, requireEboot: false) is { IsAddon: true } addon ? addon : null;

	// The installed DLC of a game (folders with its title ID and an add-on param.json), for the emulator's --addcont.
	public static List<Game> Addons(IEnumerable<string> roots, string titleId) =>
	    roots.Where(Directory.Exists).SelectMany(root => Candidates(root, 4))
	        .Select(dir => Read(dir, requireEboot: false))
	        .OfType<Game>()
	        .Where(g => g.IsAddon && g.ContentId.Length >= 16 && string.Equals(g.TitleId, titleId, StringComparison.OrdinalIgnoreCase))
	        .DistinctBy(g => g.ContentId)
	        .ToList();

	private static Game? Read(string dir, bool requireEboot = true)
	{
		var param = Path.Combine(dir, "sce_sys", "param.json");
		if (!File.Exists(param) || (requireEboot && !File.Exists(Path.Combine(dir, "eboot.bin")))) return null;
		try
		{
			using var json = JsonDocument.Parse(File.ReadAllText(param));
			var root    = json.RootElement;
			var titleId = root.TryGetProperty("titleId", out var id) ? id.GetString() ?? "" : "";
			var version = root.TryGetProperty("contentVersion", out var v) ? v.GetString() ?? "" : "";
			var content = root.TryGetProperty("contentId", out var c) ? c.GetString() ?? "" : "";
			return new Game(titleId, TitleName(root) ?? titleId, version, dir)
			{
				IsAddon = !root.TryGetProperty("applicationCategoryType", out _) && !File.Exists(Path.Combine(dir, "eboot.bin")), ContentId = content,
			};
		}
		catch (JsonException)
		{
			return null;
		}
	}

	private static string? TitleName(JsonElement root)
	{
		if (!root.TryGetProperty("localizedParameters", out var localized)) return null;
		var preferred = new List<string> { "es-419", L.T("es-ES"), "en-US" };
		if (localized.TryGetProperty("defaultLanguage", out var language) && language.GetString() is { } lang)
		{
			preferred.Add(lang);
		}
		foreach (var key in preferred)
		{
			if (localized.TryGetProperty(key, out var entry) && entry.TryGetProperty("titleName", out var name))
			{
				return name.GetString();
			}
		}
		return null;
	}
}
