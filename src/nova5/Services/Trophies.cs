using System.Text.Json;

namespace Nova5.Services;

public enum TrophyGrade { Platinum = 1, Gold, Silver, Bronze }

public sealed record Trophy(int Id, TrophyGrade Grade, string Name, string Description, bool Hidden, byte[]? Icon, DateTime? Unlocked);

public sealed record TrophySet(Game Game, string Title, IReadOnlyList<Trophy> Trophies)
{
	public int Earned  => Trophies.Count(t => t.Unlocked is not null);
	public int Percent => Trophies.Count == 0 ? 0 : (int)Math.Round(100.0 * Earned / Trophies.Count);
	public DateTime? LastUnlock => Trophies.Max(t => t.Unlocked);

	public (int Earned, int Total) Count(TrophyGrade grade) =>
	    (Trophies.Count(t => t.Grade == grade && t.Unlocked is not null), Trophies.Count(t => t.Grade == grade));
}

// A game's trophy list from sce_sys/trophy2/trophy00.ucp (its tropconf.json, tropmeta_<locale>.json and
// tropNNNN.png), with what the emulator saved in _Trophies/<title id>/trophies_<user>_0.json — the same files
// the emulator's trophy system reads and writes.
public static class Trophies
{
	private const long MaxPackage = 1L << 30;

	public static TrophySet? Load(string baseDir, Game game, string userId, string consoleLanguage)
	{
		var package = Path.Combine(game.SceSys, "trophy2", "trophy00.ucp");
		if (!File.Exists(package) || new FileInfo(package).Length > MaxPackage) return null;
		try
		{
			var files = ReadFiles(File.ReadAllBytes(package));
			if (files is null || !files.TryGetValue("tropconf.json", out var confBytes)) return null;
			using var conf = JsonDocument.Parse(confBytes);
			var fallback = conf.RootElement.TryGetProperty("defaultLanguage", out var language) ? language.GetString() : null;
			var meta = Locales(consoleLanguage).Append(fallback)
			    .Select(l => l is null ? null : files.GetValueOrDefault($"tropmeta_{l}.json"))
			    .FirstOrDefault(b => b is not null);
			if (meta is null) return null;
			using var metaDoc = JsonDocument.Parse(meta);
			var texts = metaDoc.RootElement.GetProperty("metadata");
			var title = texts.GetProperty("titleMetadata").GetProperty("name").GetString() ?? game.Name;
			var names = new Dictionary<int, (string Name, string Detail)>();
			foreach (var text in texts.GetProperty("trophyMetadata").EnumerateArray())
			{
				if (ReadId(text, "id") is { } id)
					names[id] = (text.TryGetProperty("name", out var n) ? n.GetString() ?? "" : "",
					             text.TryGetProperty("detail", out var d) ? d.GetString() ?? "" : "");
			}
			var unlocks = LoadUnlocks(Path.Combine(baseDir, "_Trophies", game.TitleId, $"trophies_{userId}_0.json"));
			var list = new List<Trophy>();
			foreach (var definition in conf.RootElement.GetProperty("trophies").EnumerateArray())
			{
				if (ReadId(definition, "id") is not { } id || !definition.TryGetProperty("grade", out var grade)) continue;
				var index = "PGSB".IndexOf(grade.GetString() ?? "?", StringComparison.Ordinal);
				if (index < 0) continue;
				var (name, detail) = names.GetValueOrDefault(id, (L.F("Trofeo {0}", id), ""));
				list.Add(new Trophy(id, (TrophyGrade)(index + 1), name, detail,
				    definition.TryGetProperty("hidden", out var hidden) && hidden.ValueKind == JsonValueKind.True,
				    files.GetValueOrDefault($"trop{id:0000}.png"), unlocks.GetValueOrDefault(id)));
			}
			return list.Count == 0 ? null : new TrophySet(game, title, list);
		}
		catch (Exception e) when (e is JsonException or IOException or KeyNotFoundException or InvalidOperationException or UnauthorizedAccessException)
		{
			return null;
		}
	}

	// The console language number Nova5.ini stores (1 = English US … ) has no simple locale name everywhere, so
	// Spanish variants are tried first, as the launcher is in Spanish, then English.
	private static IEnumerable<string> Locales(string consoleLanguage) => consoleLanguage switch
	{
		"3" => new[] { L.T("es-ES"), "es-419" },
		"20" => new[] { "es-419", L.T("es-ES") },
		_ => new[] { "es-419", L.T("es-ES"), "en-US", "en-GB" },
	};

	private static int? ReadId(JsonElement element, string property)
	{
		if (!element.TryGetProperty(property, out var value)) return null;
		if (value.ValueKind == JsonValueKind.Number && value.TryGetInt32(out var number)) return number;
		return value.ValueKind == JsonValueKind.String && int.TryParse(value.GetString(), out var text) ? text : null;
	}

	// Unlock times are microseconds since 0001-01-01 (the console's RTC ticks).
	private static Dictionary<int, DateTime?> LoadUnlocks(string path)
	{
		var result = new Dictionary<int, DateTime?>();
		if (!File.Exists(path)) return result;
		try
		{
			using var json = JsonDocument.Parse(File.ReadAllBytes(path));
			var times = json.RootElement.TryGetProperty("unlockedAt", out var at) ? at : default;
			foreach (var value in json.RootElement.GetProperty("unlockedTrophies").EnumerateArray())
			{
				if (!value.TryGetInt32(out var id)) continue;
				DateTime? when = DateTime.MinValue;
				if (times.ValueKind == JsonValueKind.Object && times.TryGetProperty(id.ToString(), out var tick) &&
				    tick.TryGetInt64(out var micro) && micro > 0 && micro < DateTime.MaxValue.Ticks / 10)
					when = new DateTime(micro * 10, DateTimeKind.Utc).ToLocalTime();
				result[id] = when;
			}
		}
		catch (Exception e) when (e is JsonException or IOException or KeyNotFoundException or InvalidOperationException)
		{
		}
		return result;
	}

	// The .ucp container: big-endian magic b228c60a, version 1, total size, file count and table offset; each
	// table entry holds a 32-byte name, an offset and a length.
	private static Dictionary<string, byte[]>? ReadFiles(byte[] data)
	{
		static ulong Be(byte[] d, int at, int size)
		{
			ulong value = 0;
			for (var i = 0; i < size; i++) value = (value << 8) | d[at + i];
			return value;
		}
		if (data.Length < 0x40 || Be(data, 0, 4) != 0xb228c60a || Be(data, 4, 4) != 1) return null;
		var size  = Be(data, 8, 8);
		var count = Be(data, 0x10, 4);
		var toc   = Be(data, 0x14, 4);
		if (size < 0x40 || size > (ulong)data.Length || count > 4096 || toc > size || 0x20 + count * 0x40 > size - toc) return null;
		var files = new Dictionary<string, byte[]>(StringComparer.Ordinal);
		for (ulong i = 0; i < count; i++)
		{
			var entry = (int)(toc + 0x20 + i * 0x40);
			var end   = Array.IndexOf(data, (byte)0, entry, 0x20);
			var name  = System.Text.Encoding.UTF8.GetString(data, entry, (end < 0 ? entry + 0x20 : end) - entry);
			var offset = Be(data, entry + 0x20, 8);
			var length = Be(data, entry + 0x28, 8);
			if (offset > size || length > size - offset) return null;
			files[name] = data.AsSpan((int)offset, (int)length).ToArray();
		}
		return files;
	}
}
