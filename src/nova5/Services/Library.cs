using System.Text.Json;

namespace Nova5.Services;

public sealed record Game(string TitleId, string Name, string Version, string Dir)
{
	public string  Eboot      => Path.Combine(Dir, "eboot.bin");
	public string  SceSys     => Path.Combine(Dir, "sce_sys");
	public string? Icon       => FirstExisting("icon0.png");
	public string? Background => FirstExisting("pic0.png", "pic1.png", "pic2.png");
	public string? Music      => FirstExisting("snd0.at9");

	private string? FirstExisting(params string[] names) =>
	    names.Select(n => Path.Combine(SceSys, n)).FirstOrDefault(File.Exists);
}

// Finds installed PS5 games: folders with eboot.bin and sce_sys/param.json, up to three levels deep.
public static class Library
{
	public static List<Game> Scan(IEnumerable<string> roots)
	{
		var games = new Dictionary<string, Game>(StringComparer.OrdinalIgnoreCase);
		foreach (var root in roots.Where(Directory.Exists))
		{
			foreach (var dir in Candidates(root, 3))
			{
				if (Read(dir) is { } game) games.TryAdd(Path.GetFullPath(game.Dir), game);
			}
		}
		return games.Values.OrderBy(g => g.Name, StringComparer.CurrentCultureIgnoreCase).ToList();
	}

	private static IEnumerable<string> Candidates(string dir, int depth)
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
			var name = Path.GetFileName(child);
			if (name is "sce_sys" or "sce_module" or "data") continue;
			foreach (var nested in Candidates(child, depth - 1)) yield return nested;
		}
	}

	private static Game? Read(string dir)
	{
		var param = Path.Combine(dir, "sce_sys", "param.json");
		if (!File.Exists(param) || !File.Exists(Path.Combine(dir, "eboot.bin"))) return null;
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
