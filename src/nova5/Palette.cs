using Avalonia;
using Avalonia.Media;

namespace Nova5;

// Nova5 identity: deep night background and an aurora gradient from teal to violet.
public static class Palette
{
	public static readonly Color Night   = Color.Parse("#070A16");
	public static readonly Color Ink     = Color.Parse("#0D1230");
	public static readonly Color AuroraA = Color.Parse("#5EEAD4");
	public static readonly Color AuroraB = Color.Parse("#8B5CF6");
	public static readonly Color Text    = Color.Parse("#F4F6FF");
	public static readonly Color Muted   = Color.Parse("#A9B0D6");

	public static readonly FontFamily Font = new("Segoe UI Variable Display, Segoe UI, Inter, Noto Sans, sans-serif");

	public static IBrush Brush(Color color) => new SolidColorBrush(color);
	public static IBrush Brush(Color color, double alpha) => new SolidColorBrush(color, alpha);

	public static LinearGradientBrush Aurora(double alpha = 1) => new()
	{
		StartPoint    = new RelativePoint(0, 0, RelativeUnit.Relative),
		EndPoint      = new RelativePoint(1, 0, RelativeUnit.Relative),
		GradientStops = { new GradientStop(AuroraA, 0), new GradientStop(AuroraB, 1) },
		Opacity       = alpha,
	};

	public static LinearGradientBrush Fade(RelativePoint start, RelativePoint end, params (double Offset, double Alpha)[] stops)
	{
		var brush = new LinearGradientBrush { StartPoint = start, EndPoint = end };
		foreach (var (offset, alpha) in stops) brush.GradientStops.Add(new GradientStop(Color.FromArgb((byte)(alpha * 255), Night.R, Night.G, Night.B), offset));
		return brush;
	}

	public static RelativePoint P(double x, double y) => new(x, y, RelativeUnit.Relative);
}
