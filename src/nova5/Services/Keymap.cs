using Avalonia.Input;

namespace Nova5.Services;

// The keyboard and mouse map the emulator takes as --keymap Control=Input (graphics/presentation/window/hostInput.cpp):
// a map replaces the emulator's built-in keys, so Nova5 always passes the whole map. Kept in Nova5.ini as
// "keymap = Cross=J,Circle=L,..." plus "mouse_sensitivity".
public static class Keymap
{
	public sealed record Control(string Name, string Label);

	public static readonly Control[] Controls =
	{
		new("Cross", L.T("Cruz (✕)")), new("Circle", L.T("Círculo (○)")), new("Square", L.T("Cuadrado (□)")), new("Triangle", L.T("Triángulo (△)")),
		new("L1", "L1"), new("R1", "R1"), new("L2", "L2"), new("R2", "R2"), new("L3", L.T("L3 (pulsar stick izquierdo)")), new("R3", L.T("R3 (pulsar stick derecho)")),
		new("Up", L.T("Cruceta arriba")), new("Down", L.T("Cruceta abajo")), new("Left", L.T("Cruceta izquierda")), new("Right", L.T("Cruceta derecha")),
		new("LeftStickUp", L.T("Stick izquierdo arriba")), new("LeftStickDown", L.T("Stick izquierdo abajo")),
		new("LeftStickLeft", L.T("Stick izquierdo a la izquierda")), new("LeftStickRight", L.T("Stick izquierdo a la derecha")),
		new("RightStickUp", L.T("Stick derecho arriba")), new("RightStickDown", L.T("Stick derecho abajo")),
		new("RightStickLeft", L.T("Stick derecho a la izquierda")), new("RightStickRight", L.T("Stick derecho a la derecha")),
		new("Options", "Options"), new("TouchPad", L.T("Panel táctil (izquierda)")), new("TouchPadRight", L.T("Panel táctil (derecha)")),
	};

	// The emulator's own keys, plus L2 and R2 (Z and C, as in KytyPS5's launcher), which it has no default for.
	public static readonly Dictionary<string, string> Defaults = new()
	{
		["Cross"] = "J", ["Circle"] = "L", ["Square"] = "K", ["Triangle"] = "I", ["L1"] = "Q", ["R1"] = "E", ["L2"] = "Z", ["R2"] = "C",
		["L3"] = "Left Shift", ["R3"] = "Left Ctrl", ["Up"] = "Up", ["Down"] = "Down", ["Left"] = "Left", ["Right"] = "Right",
		["LeftStickUp"] = "W", ["LeftStickDown"] = "S", ["LeftStickLeft"] = "A", ["LeftStickRight"] = "D",
		["RightStickUp"] = "T", ["RightStickDown"] = "G", ["RightStickLeft"] = "F", ["RightStickRight"] = "H",
		["Options"] = "Return", ["TouchPad"] = "Backspace", ["TouchPadRight"] = "Tab",
	};

	public static Dictionary<string, string> Load(string? saved)
	{
		var map = new Dictionary<string, string>(Defaults);
		if (string.IsNullOrWhiteSpace(saved)) return map;
		foreach (var entry in saved.Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
		{
			var split = entry.IndexOf('=');
			if (split > 0 && Controls.Any(c => c.Name == entry[..split])) map[entry[..split]] = entry[(split + 1)..];
		}
		return map;
	}

	public static string Save(Dictionary<string, string> map) =>
	    string.Join(",", Controls.Where(c => map.GetValueOrDefault(c.Name) is { Length: > 0 }).Select(c => $"{c.Name}={map[c.Name]}"));

	// What a key looks like on screen ("Mouse:Left" → "Clic izquierdo").
	public static string Show(string input) => input switch
	{
		"" => L.T("Sin asignar"), "Mouse:Left" => L.T("Clic izquierdo"), "Mouse:Right" => L.T("Clic derecho"), "Mouse:Middle" => L.T("Clic central"),
		"Mouse:X1" => L.T("Ratón atrás"), "Mouse:X2" => L.T("Ratón adelante"), "Mouse:WheelUp" => L.T("Rueda arriba"), "Mouse:WheelDown" => L.T("Rueda abajo"),
		"Return" => "Enter", "Space" => L.T("Espacio"), "Left Shift" => L.T("Mayús"), "Left Ctrl" => "Ctrl", "Left Alt" => "Alt",
		"Up" => "↑", "Down" => "↓", "Left" => "←", "Right" => "→", "Backspace" => L.T("Retroceso"), "Tab" => "Tab",
		_ => input,
	};

	// The SDL key name for an Avalonia key, or null for one the emulator keeps (F7, F8, F11, Esc) or cannot name.
	public static string? Name(Key key) => key switch
	{
		>= Key.A and <= Key.Z => ((char)('A' + (key - Key.A))).ToString(),
		>= Key.D0 and <= Key.D9 => ((char)('0' + (key - Key.D0))).ToString(),
		>= Key.NumPad0 and <= Key.NumPad9 => $"Keypad {key - Key.NumPad0}",
		>= Key.F1 and <= Key.F12 when key is not (Key.F7 or Key.F8 or Key.F11) => $"F{key - Key.F1 + 1}",
		Key.Space => "Space", Key.Enter => "Return", Key.Tab => "Tab", Key.Back => "Backspace",
		Key.LeftShift or Key.RightShift => "Left Shift", Key.LeftCtrl or Key.RightCtrl => "Left Ctrl", Key.LeftAlt or Key.RightAlt => "Left Alt",
		Key.Up => "Up", Key.Down => "Down", Key.Left => "Left", Key.Right => "Right",
		Key.Insert => "Insert", Key.Home => "Home", Key.End => "End", Key.PageUp => "PageUp", Key.PageDown => "PageDown",
		Key.OemComma => ",", Key.OemPeriod => ".", Key.OemMinus => "-", Key.OemPlus => "=", Key.OemSemicolon => ";",
		Key.OemQuotes => "'", Key.OemOpenBrackets => "[", Key.OemCloseBrackets => "]", Key.OemPipe => "\\", Key.OemQuestion => "/",
		Key.OemTilde => "`",
		_ => null,
	};
}
