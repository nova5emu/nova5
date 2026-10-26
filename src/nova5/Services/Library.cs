using System.Text.Json;
using System.Text.RegularExpressions;

namespace Nova5.Services;

public sealed record Game(string TitleId, string Name, string Version, string Dir)
{
	// Set for a .pkg that is not installed yet; Dir then holds the package's sce_sys read ahead for its art.
	public string? Package { get; init; }

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
		foreach (var root in roots.Where(Directory.Exists))
		{
			foreach (var dir in Candidates(root, 3))
			{
				if (Read(dir) is { } game) games.TryAdd(Path.GetFullPath(game.Dir), game);
				try
				{
					packages.AddRange(Directory.GetFiles(dir, "*.pkg"));
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
			if (info is not null && installed.Contains(info.TitleId)) continue;
			pending.Add(info is null
			    ? new Game("", PackageName(package), "", Path.GetDirectoryName(package)!) { Package = package }
			    : info with { Package = package });
		}
		return games.Values.OrderBy(g => g.Name, StringComparer.CurrentCultureIgnoreCase).Concat(pending).ToList();
	}

	private static string PackageName(string package)
	{
		var name = Path.GetFileNameWithoutExtension(package);
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
			return new Game(titleId, TitleName(root) ?? titleId, version, dir);
		}
		catch (JsonException)
		{
			return null;
		}
	}

	private static string? TitleName(JsonElement root)
	{
		if (!root.TryGetProperty("localizedParameters", out var localized)) return null;
		var preferred = new List<string> { "es-419", "es-ES", "en-US" };
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
