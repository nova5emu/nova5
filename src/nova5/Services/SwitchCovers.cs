using System.Globalization;
using System.Text;

namespace Nova5.Services;

// Covers for known Switch 2 games, shipped with the launcher (the same ones as NovaS's web page), so a game the player
// adds gets its cover without doing anything. A game matches when its name contains one of the entry's names, compared
// without accents, spaces or punctuation (so "DonkeyKongBananza" and "Donkey Kong Bananza" are the same).
public static class SwitchCovers
{
	private sealed record Entry(string Id, string Name, params string[] Aliases);

	// Most specific names first: the first match wins.
	private static readonly Entry[] Catalog =
	{
		new("zelda-ocarina-of-time", "The Legend of Zelda: Ocarina of Time", "Ocarina of Time"),
		new("hyrule-warriors-age-of-imprisonment", "Hyrule Warriors: Age of Imprisonment", "Age of Imprisonment", "Era del destierro", "Hyrule Warriors"),
		new("mario-kart-world", "Mario Kart World", "Mario Kart World"),
		new("donkey-kong-bananza", "Donkey Kong Bananza", "Donkey Kong Bananza", "DK Bananza"),
		new("pokemon-legends-z-a", "Pokémon Legends: Z-A", "Legends Z-A", "Leyendas Z-A", "Legends ZA", "Leyendas ZA"),
		new("pokemon-pokopia", "Pokémon Pokopia", "Pokopia"),
		new("nintendo-switch-sports-resort", "Nintendo Switch Sports Resort", "Switch Sports Resort", "Sports Resort"),
		new("splatoon-raiders", "Splatoon Raiders", "Splatoon Raiders"),
		new("kirby-air-riders", "Kirby Air Riders", "Kirby Air Riders", "Air Riders"),
		new("super-mario-bros-wonder", "Super Mario Bros. Wonder", "Mario Bros Wonder", "Mario Wonder"),
		new("mario-tennis-fever", "Mario Tennis Fever", "Mario Tennis Fever"),
		new("super-mario-galaxy-2", "Super Mario Galaxy 2", "Mario Galaxy 2", "Mario Galaxy"),
		new("fire-emblem-fortunes-weave", "Fire Emblem: Fortune's Weave", "Fortune's Weave", "Fortunes Weave"),
		new("yoshi-and-the-mysterious-book", "Yoshi and the Mysterious Book", "Mysterious Book", "Libro misterioso", "Yoshi"),
		new("pragmata", "Pragmata", "Pragmata"),
		new("cyberpunk-2077", "Cyberpunk 2077: Ultimate Edition", "Cyberpunk"),
		new("deltarune", "Deltarune", "Deltarune"),
		new("fast-fusion", "Fast Fusion", "Fast Fusion"),
		new("red-dead-redemption", "Red Dead Redemption", "Red Dead Redemption"),
		new("resident-evil-4", "Resident Evil 4", "Resident Evil 4", "Biohazard 4"),
		new("metroid-prime-4-beyond", "Metroid Prime 4: Beyond", "Metroid Prime 4"),
		new("star-fox", "Star Fox", "Star Fox", "Starfox"),
		new("drag-x-drive", "Drag x Drive", "Drag x Drive", "Drag Drive"),
	};

	// The known game's tidy name and cover file (unpacked once into _Cache\novas-covers\builtin), or null.
	public static (string Name, string Cover)? Find(string gameName)
	{
		var key = Key(gameName);
		foreach (var entry in Catalog)
		{
			if (!entry.Aliases.Any(alias => key.Contains(Key(alias), StringComparison.Ordinal))) continue;
			var file = Path.Combine(AppContext.BaseDirectory, "_Cache", "novas-covers", "builtin", entry.Id + ".webp");
			try
			{
				if (!File.Exists(file))
				{
					Directory.CreateDirectory(Path.GetDirectoryName(file)!);
					using var source = Avalonia.Platform.AssetLoader.Open(new Uri($"avares://Nova5/Assets/novas-covers/{entry.Id}.webp"));
					using var target = File.Create(file);
					source.CopyTo(target);
				}
			}
			catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidOperationException)
			{
				return null;
			}
			return (entry.Name, file);
		}
		return null;
	}

	// "Pokémon Legends: Z-A" -> "pokemonlegendsza".
	private static string Key(string text)
	{
		var plain = new StringBuilder();
		foreach (var c in text.Normalize(NormalizationForm.FormD))
		{
			if (CharUnicodeInfo.GetUnicodeCategory(c) == UnicodeCategory.NonSpacingMark) continue;
			if (char.IsLetterOrDigit(c)) plain.Append(char.ToLowerInvariant(c));
		}
		return plain.ToString();
	}
}
