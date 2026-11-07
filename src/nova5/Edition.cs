namespace Nova5;

// The same launcher ships twice: as Nova5 (PS5, with the NovaS tab) and as NovaS on its own (Switch 2 only), told
// apart by the executable's name (NovaS.exe) or by --novas. NovaS opens straight on its home, has no PS5 side and
// updates from its own repository.
public static class Edition
{
	public static bool NovaSOnly { get; set; } =
	    Path.GetFileNameWithoutExtension(Environment.ProcessPath ?? "").StartsWith("NovaS", StringComparison.OrdinalIgnoreCase);

	public static string Name => NovaSOnly ? "NovaS" : "Nova5";
	public static string Repository => NovaSOnly ? "nova5emu/novas" : "nova5emu/nova5";
	public static string SetupAsset => NovaSOnly ? "NovaS-Setup.exe" : "Nova5-Setup.exe";
}
