namespace Nova5.Services;

// Ajustes > Configurar mando: which button of the player's pad does each Switch button, for all games or for one.
// Pad buttons are named by their place on an Xbox-style pad (see Gamepad.PadButtons). The maps live in
// novas-choices.ini, [Mando]: "map" for all games and "map.<game>" for one, and reach the engine as
// NOVAS_BUTTON_MAP ("A=south;B=east;..."; see the engine's auto_controller.cpp).
public static class PadMap
{
	public sealed record SwitchButton(string Id, string Name);

	public static readonly SwitchButton[] Buttons =
	{
		new("A", "A"), new("B", "B"), new("X", "X"), new("Y", "Y"),
		new("L", "L"), new("R", "R"), new("ZL", "ZL"), new("ZR", "ZR"),
		new("Plus", "+"), new("Minus", "−"), new("LStick", L.T("Clic stick izq.")), new("RStick", L.T("Clic stick der.")),
		new("DUp", L.T("Cruceta ▲")), new("DDown", L.T("Cruceta ▼")), new("DLeft", L.T("Cruceta ◀")), new("DRight", L.T("Cruceta ▶")),
	};

	// Tipo Switch: like on the console, the right face button is A.
	public static readonly Dictionary<string, string> ByPosition = new()
	{
		["A"] = "east", ["B"] = "south", ["X"] = "north", ["Y"] = "west", ["L"] = "lb", ["R"] = "rb", ["ZL"] = "lt", ["ZR"] = "rt",
		["Plus"] = "start", ["Minus"] = "back", ["LStick"] = "ls", ["RStick"] = "rs",
		["DUp"] = "up", ["DDown"] = "down", ["DLeft"] = "left", ["DRight"] = "right",
	};

	// Tipo Xbox: the button with the letter A is A (in Donkey Kong Bananza, A jumps, as in an Xbox game).
	public static readonly Dictionary<string, string> ByLabel = new(ByPosition)
	{
		["A"] = "south", ["B"] = "east", ["X"] = "west", ["Y"] = "north",
	};

	private const string Section = "Mando";
	private static string KeyFor(string? game) => game is null ? "map" : "map." + game;

	// ---- What each Switch button does in the games NovaS knows (from their default controls) ------------------

	public sealed record GameControls(string Key, string Name, string[] Match, Dictionary<string, string> Actions);

	public static readonly GameControls[] Games =
	{
		new("donkey-kong-bananza", "Donkey Kong Bananza", new[] { "bananza" }, new()
		{
			["A"] = L.T("Saltar"), ["B"] = L.T("Golpe abajo"), ["X"] = L.T("Golpe arriba"), ["Y"] = L.T("Golpear"),
			["L"] = L.T("Música"), ["R"] = L.T("Palmada"), ["ZL"] = L.T("Rodar"), ["ZR"] = L.T("Agarrar"),
			["Plus"] = L.T("Pausa"), ["Minus"] = L.T("Mapa"), ["DUp"] = L.T("Habilidades"), ["DDown"] = L.T("Foto"),
			["DLeft"] = L.T("Bananza ◀"), ["DRight"] = L.T("Bananza ▶"),
		}),
		new("zelda-ocarina-of-time", "The Legend of Zelda: Ocarina of Time", new[] { "ocarina" }, new()
		{
			["A"] = L.T("Acción"), ["B"] = L.T("Guardar"), ["X"] = L.T("Saltar"), ["Y"] = L.T("Atacar"),
			["L"] = L.T("Objeto"), ["R"] = L.T("Objeto"), ["ZL"] = L.T("Fijar"), ["ZR"] = L.T("Usar objeto"),
			["Plus"] = L.T("Menú"), ["Minus"] = L.T("Mapa"), ["RStick"] = L.T("Cámara"),
			["DLeft"] = L.T("Ocarina"), ["DRight"] = L.T("Navi"),
		}),
		new("pokemon-pokopia", "Pokémon Pokopia", new[] { "pokopia" }, new()
		{
			["A"] = L.T("Interactuar"), ["B"] = L.T("Guardar"), ["X"] = L.T("Mochila"), ["Y"] = L.T("Recoger"),
			["L"] = L.T("Elegir mov."), ["R"] = L.T("Saltar"), ["ZL"] = L.T("De lado"), ["ZR"] = L.T("Usar mov."),
			["Plus"] = L.T("Menú"), ["Minus"] = L.T("Foto"), ["LStick"] = L.T("Objetivo"), ["RStick"] = L.T("Bordes"),
			["DUp"] = L.T("Seguir"), ["DDown"] = L.T("Gestos"), ["DLeft"] = L.T("Mov. ◀"), ["DRight"] = L.T("Mov. ▶"),
		}),
	};

	// The key a game's own map is kept under: the known game's, else its name in plain letters.
	public static string GameKey(string name)
	{
		var lower = name.ToLowerInvariant();
		if (Games.FirstOrDefault(g => g.Match.Any(lower.Contains)) is { } known) return known.Key;
		var slug = new string(lower.Select(c => char.IsLetterOrDigit(c) ? c : '-').ToArray());
		while (slug.Contains("--")) slug = slug.Replace("--", "-");
		return slug.Trim('-');
	}

	public static Dictionary<string, string>? Actions(string? game) => Games.FirstOrDefault(g => g.Key == game)?.Actions;

	// The game's proper name when NovaS knows it (a folder can be called "pokopia v2-0-1").
	public static string DisplayName(string name) => Games.FirstOrDefault(g => g.Key == GameKey(name))?.Name ?? name;

	// ---- The maps -----------------------------------------------------------------------------------------------

	// The player's own map (for one game, or for all with null), or null.
	public static Dictionary<string, string>? Custom(string? game = null)
	{
		var text = new IniFile(SwitchGames.ChoicesPath).Get(Section, KeyFor(game));
		if (string.IsNullOrWhiteSpace(text)) return null;
		var map = new Dictionary<string, string>(ByPosition);
		foreach (var item in text.Split(';', StringSplitOptions.RemoveEmptyEntries))
		{
			var parts = item.Split('=', 2);
			if (parts.Length == 2 && map.ContainsKey(parts[0]) && ByPosition.ContainsValue(parts[1])) map[parts[0]] = parts[1];
		}
		return map;
	}

	// What a game will use: its own map, else the one for all games, else the style of Botones del mando.
	public static Dictionary<string, string> Current(string? game = null)
	{
		if (game is not null && Custom(game) is { } own) return own;
		if (Custom() is { } custom) return custom;
		var labels = SwitchGames.Options.First(o => o.Key == "NOVAS_BUTTON_LABELS");
		return labels.Choices[SwitchGames.Current(labels)].Value == "1" ? new(ByLabel) : new(ByPosition);
	}

	// NOVAS_BUTTON_MAP for a game: its own map, else the one for all games (null: the style decides).
	public static string? EnvironmentValue(string? game = null)
	{
		var ini = new IniFile(SwitchGames.ChoicesPath);
		if (game is not null && ini.Get(Section, KeyFor(game)) is { Length: > 0 } own) return own;
		return ini.Get(Section, KeyFor(null)) is { Length: > 0 } all ? all : null;
	}

	public static void Save(Dictionary<string, string> map, string? game = null)
	{
		Directory.CreateDirectory(Path.GetDirectoryName(SwitchGames.ChoicesPath)!);
		var ini = new IniFile(SwitchGames.ChoicesPath);
		ini.Set(Section, KeyFor(game), string.Join(";", Buttons.Select(b => $"{b.Id}={map[b.Id]}")));
		ini.Save();
	}

	// For all games: back to a style of Ajustes > Botones del mando (the map for all games is forgotten).
	public static void UseStyle(bool byLabel)
	{
		var labels = SwitchGames.Options.First(o => o.Key == "NOVAS_BUTTON_LABELS");
		SwitchGames.Set(labels, Array.FindIndex(labels.Choices, c => c.Value == (byLabel ? "1" : "0")));
	}

	public static void Forget(string? game = null)
	{
		var ini = new IniFile(SwitchGames.ChoicesPath);
		ini.Remove(Section, KeyFor(game));
		ini.Save();
	}

	// The Switch button in a few letters, for the tags on the drawing.
	public static string ShortName(string id) => id switch
	{
		"Plus" => "+", "Minus" => "−", "LStick" => L.T("Stick L"), "RStick" => L.T("Stick R"),
		"DUp" => "▲", "DDown" => "▼", "DLeft" => "◀", "DRight" => "▶", _ => id,
	};

	// How a pad button is called on screen; face buttons show what is printed on the player's pad.
	public static string PadName(string place, Gamepad? pad) => place switch
	{
		"south" or "east" or "west" or "north" => pad?.FaceLabel(place) ?? (place switch { "south" => "A", "east" => "B", "west" => "X", _ => "Y" }),
		"lb" => "LB", "rb" => "RB", "lt" => "LT", "rt" => "RT",
		"start" => L.T("Menú"), "back" => L.T("Vista"),
		"ls" => L.T("Stick izq."), "rs" => L.T("Stick der."),
		"up" => "▲", "down" => "▼", "left" => "◀", "right" => "▶",
		_ => place,
	};
}
