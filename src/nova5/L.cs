using System.Globalization;

namespace Nova5;

// The launcher's language. Texts are written in Spanish in the code; in English they are looked up in Strings.En
// (keyed by the Spanish text, with {0}, {1}... where interpolated values go). A missing entry stays in Spanish.
public static class L
{
	public static bool English { get; private set; }

	// "auto": Spanish when Windows is in Spanish, else English. "es" / "en": as chosen in Nova5's or NovaS's settings.
	public static void Init(string? setting) =>
	    English = setting switch
	    {
		    "es" => false,
		    "en" => true,
		    _    => !CultureInfo.CurrentUICulture.TwoLetterISOLanguageName.Equals("es", StringComparison.OrdinalIgnoreCase),
	    };

	public static string T(string spanish) => English && Strings.En.TryGetValue(spanish, out var english) ? english : spanish;

	public static string F(string spanish, params object?[] values) => string.Format(CultureInfo.CurrentCulture, T(spanish), values);
}
