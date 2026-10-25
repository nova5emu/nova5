using System.Text;

namespace Nova5.Services;

// Reads and edits Nova5.ini (INI) line by line, so keys Nova5 does not know survive a save.
public sealed class IniFile
{
	private readonly string       _path;
	private readonly List<string> _lines;

	public IniFile(string path)
	{
		_path  = path;
		_lines = File.Exists(path) ? File.ReadAllLines(path, Encoding.UTF8).ToList() : new List<string>();
	}

	public string? Get(string section, string key)
	{
		var index = Find(section, key);
		if (index < 0) return null;
		var line = _lines[index];
		return line[(line.IndexOf('=') + 1)..];
	}

	public bool GetBool(string section, string key, bool fallback) =>
	    Get(section, key) is { } value ? value.Equals("true", StringComparison.OrdinalIgnoreCase) : fallback;

	public void Set(string section, string key, string value)
	{
		var index = Find(section, key);
		if (index >= 0)
		{
			_lines[index] = $"{key}={value}";
			return;
		}
		var header = _lines.FindIndex(l => l.Trim() == $"[{section}]");
		if (header < 0)
		{
			if (_lines.Count > 0 && _lines[^1].Length != 0) _lines.Add("");
			_lines.Add($"[{section}]");
			_lines.Add($"{key}={value}");
			return;
		}
		var end = header + 1;
		while (end < _lines.Count && !_lines[end].TrimStart().StartsWith('[')) end++;
		while (end > header + 1 && _lines[end - 1].Trim().Length == 0) end--;
		_lines.Insert(end, $"{key}={value}");
	}

	public void Save() => File.WriteAllLines(_path, _lines, new UTF8Encoding(false));

	private int Find(string section, string key)
	{
		var inside = false;
		for (var i = 0; i < _lines.Count; i++)
		{
			var line = _lines[i].Trim();
			if (line.StartsWith('['))
			{
				inside = line == $"[{section}]";
				continue;
			}
			if (inside && line.StartsWith(key + "=", StringComparison.Ordinal)) return i;
		}
		return -1;
	}
}
