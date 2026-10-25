using System.Diagnostics;
using System.Net.Http.Headers;
using System.Reflection;
using System.Security.Cryptography;
using System.Text.Json;

namespace Nova5.Services;

public sealed record UpdateInfo(Version Version, string Url, long Size, string? Sha256);

// Checks the latest GitHub release of Nova5 and installs it with the release's own setup.
public static class Updates
{
	private const string LatestRelease = "https://api.github.com/repos/nova5emu/nova5/releases/latest";
	private const string SetupAsset    = "Nova5-Setup.exe";

	public static Version Current { get; } =
	    Assembly.GetEntryAssembly()?.GetName().Version is { } v ? new Version(v.Major, v.Minor, Math.Max(v.Build, 0)) : new Version(0, 0, 0);

	private static readonly HttpClient Http = CreateClient();

	private static HttpClient CreateClient()
	{
		var client = new HttpClient { Timeout = TimeSpan.FromMinutes(10) };
		client.DefaultRequestHeaders.UserAgent.Add(new ProductInfoHeaderValue("Nova5", Current.ToString()));
		client.DefaultRequestHeaders.Accept.Add(new MediaTypeWithQualityHeaderValue("application/vnd.github+json"));
		return client;
	}

	// Null when there is no newer release, no setup asset, or no network.
	public static async Task<UpdateInfo?> CheckAsync()
	{
		try
		{
			using var response = await Http.GetAsync(LatestRelease);
			if (!response.IsSuccessStatusCode) return null;
			using var json = JsonDocument.Parse(await response.Content.ReadAsStringAsync());
			var root = json.RootElement;
			if (!Version.TryParse(root.GetProperty("tag_name").GetString()?.TrimStart('v', 'V'), out var latest)) return null;
			if (latest <= Current) return null;
			foreach (var asset in root.GetProperty("assets").EnumerateArray())
			{
				if (asset.GetProperty("name").GetString() != SetupAsset) continue;
				var digest = asset.TryGetProperty("digest", out var d) ? d.GetString() : null;
				return new UpdateInfo(latest, asset.GetProperty("browser_download_url").GetString()!,
				                      asset.GetProperty("size").GetInt64(),
				                      digest?.StartsWith("sha256:", StringComparison.Ordinal) == true ? digest[7..] : null);
			}
		}
		catch (Exception e) when (e is HttpRequestException or TaskCanceledException or JsonException or KeyNotFoundException or InvalidOperationException)
		{
		}
		return null;
	}

	// Downloads the setup to %TEMP% and checks its size and, when GitHub publishes it, its SHA-256.
	public static async Task<string> DownloadAsync(UpdateInfo update, IProgress<double> progress)
	{
		var path = Path.Combine(Path.GetTempPath(), $"Nova5-Setup-{update.Version}.exe");
		using (var response = await Http.GetAsync(update.Url, HttpCompletionOption.ResponseHeadersRead))
		{
			response.EnsureSuccessStatusCode();
			await using var input  = await response.Content.ReadAsStreamAsync();
			await using var output = File.Create(path);
			var buffer = new byte[1 << 16];
			long done  = 0;
			int read;
			while ((read = await input.ReadAsync(buffer)) > 0)
			{
				await output.WriteAsync(buffer.AsMemory(0, read));
				done += read;
				progress.Report(update.Size > 0 ? (double)done / update.Size : 0);
			}
		}
		var length = new FileInfo(path).Length;
		if (length != update.Size) throw new InvalidDataException($"Descarga incompleta ({length} de {update.Size} bytes)");
		if (update.Sha256 is { } expected)
		{
			await using var file = File.OpenRead(path);
			var actual = Convert.ToHexString(await SHA256.HashDataAsync(file));
			if (!actual.Equals(expected, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("La descarga no coincide con la publicada");
		}
		return path;
	}

	// Runs the setup silently over this installation; it closes Nova5, keeps user data and starts Nova5 again.
	public static void Install(string setup, string installDir)
	{
		var info = new ProcessStartInfo(setup) { UseShellExecute = false };
		foreach (var arg in new[] { "/SILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/CURRENTUSER", $"/DIR={installDir.TrimEnd('\\')}" })
		{
			info.ArgumentList.Add(arg);
		}
		Process.Start(info);
	}
}
