using System.Text.Json;

namespace Nova5.Services;

// Nova5's own state (play time, last session, extra folders, music), kept in nova5.json beside the launcher.
public sealed class Store
{
	public Dictionary<string, double>   PlaySeconds { get; set; } = new();
	public Dictionary<string, DateTime> LastPlayed  { get; set; } = new();
	public List<string>                 ExtraDirs   { get; set; } = new();
	public bool                         Music       { get; set; } = true;
	public double                       MusicVolume { get; set; } = 0.6;

	private static string FilePath => Path.Combine(AppContext.BaseDirectory, "nova5.json");

	public static Store Load()
	{
		try
		{
			if (File.Exists(FilePath)) return JsonSerializer.Deserialize<Store>(File.ReadAllText(FilePath)) ?? new Store();
		}
		catch (JsonException)
		{
		}
		return new Store();
	}

	public void Save() =>
	    File.WriteAllText(FilePath, JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true }));
}
