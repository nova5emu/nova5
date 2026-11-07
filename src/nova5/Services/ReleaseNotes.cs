using System.Reflection;
using System.Text.Json;

namespace Nova5.Services;

public sealed record ReleaseNote(Version Version, string Date, string Title, IReadOnlyList<string> Items);

// What each version brings, from website/novedades.json (embedded at build time, so the web and the launcher show
// the same text).
public static class ReleaseNotes
{
	public static IReadOnlyList<ReleaseNote> All { get; } = Load();

	private static List<ReleaseNote> Load()
	{
		using var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream("novedades.json");
		if (stream is null) return new();
		using var json = JsonDocument.Parse(stream);
		var notes = new List<ReleaseNote>();
		foreach (var entry in json.RootElement.GetProperty("versions").EnumerateArray())
		{
			if (!Version.TryParse(entry.GetProperty("version").GetString(), out var version)) continue;
			// In English, a version's English title and items when it has them.
			var english = L.English && entry.TryGetProperty("items_en", out _);
			notes.Add(new ReleaseNote(version, entry.GetProperty("date").GetString() ?? "", entry.GetProperty(english ? "title_en" : "title").GetString() ?? "",
			    entry.GetProperty(english ? "items_en" : "items").EnumerateArray().Select(i => i.GetString() ?? "").ToList()));
		}
		return notes.OrderByDescending(n => n.Version).ToList();
	}

	// The versions installed since the player last saw the notes (just the current one on a first run), newest first.
	public static List<ReleaseNote> Since(string? seen, Version current)
	{
		var newer = Version.TryParse(seen, out var last)
		    ? All.Where(n => n.Version > last && n.Version <= current).ToList()
		    : All.Where(n => n.Version == current).ToList();
		return newer.Count > 0 ? newer : All.Where(n => n.Version <= current).Take(1).ToList();
	}
}
