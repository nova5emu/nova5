using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Shapes;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Threading;
using Nova5.Services;

namespace Nova5;

// Ajustes > Configurar mando: a drawn pad that lights the buttons as they are pressed, and the list of the Switch's
// buttons with the pad button that does each one, for all games or for one (LB / RB choose the game). With a game
// NovaS knows, each button says what it does in that game ("A: Saltar"). A to change one, then press its button.
public sealed partial class NovaSHome
{
	public Gamepad? Pad { get; set; }

	private readonly Grid _padHost = new() { IsVisible = false, Background = new SolidColorBrush(Colors.Black, 0.6) };
	private readonly StackPanel _padRows = new() { Spacing = 10 };
	private readonly TextBlock _padStatusLine = Label("", 22, FontWeight.Normal, Dim);
	private readonly Dictionary<string, Shape> _padParts = new();
	private readonly Dictionary<string, TextBlock> _padTags = new();
	private Ellipse? _leftKnob, _rightKnob;
	private DispatcherTimer? _padTimer;
	private Dictionary<string, string> _padMap = new();
	private int _padRow;
	private bool _padCapturing;

	// After the 16 buttons: Tipo Switch, Tipo Xbox, (for one game) Como en todos, and Listo.
	private int PadActions => PadGameKey is null ? 3 : 4;

	// The games to configure: all of them first, then each game in the library (one entry per game).
	private readonly TextBlock _padGameLine = Label("", 26, FontWeight.Bold, TextColor);
	private readonly TextBlock _padGameNote = Label("", 20, FontWeight.Normal, Dim);
	private List<(string? Key, string Name)> _padGames = new();
	private int _padGame;
	private string? PadGameKey => _padGame < _padGames.Count ? _padGames[_padGame].Key : null;
	private static int PadButtonRows => PadMap.Buttons.Length;

	public bool PadSetupOpen => _padHost.IsVisible;

	// NOVAS_DEMO_PAD=1 (only for recording the tutorial): a pretend pad presses each button on the drawing, moves
	// the sticks and, when NovaS waits for a button, presses Y. No pad is needed and nothing reaches a game.
	private static readonly bool DemoPad = Environment.GetEnvironmentVariable("NOVAS_DEMO_PAD") == "1";
	private DateTime _demoStart;
	private (DateTime Until, string Place)? _demoPress;
	private bool PadPresent => DemoPad || Pad?.HasPad == true;

	private static readonly (double At, string Place)[] DemoTour =
	{
		(1.0, "south"), (1.8, "east"), (2.6, "west"), (3.4, "north"), (4.4, "lb"), (5.1, "rb"), (5.8, "lt"), (6.5, "rt"),
		(7.5, "up"), (8.1, "right"), (8.7, "down"), (9.3, "left"), (10.2, "back"), (10.9, "start"),
	};

	private HashSet<string> DemoHeld(out (double X, double Y) left, out (double X, double Y) right)
	{
		var t = (DateTime.Now - _demoStart).TotalSeconds;
		var held = new HashSet<string>(DemoTour.Where(d => t >= d.At && t < d.At + 0.45).Select(d => d.Place));
		if (_demoPress is { } press && DateTime.Now < press.Until) held.Add(press.Place);
		left = right = default;
		if (t is >= 12 and < 14.5) { var a = (t - 12) * Math.PI; left = (Math.Cos(a), Math.Sin(a)); }
		if (t is >= 14.5 and < 17) { var a = (t - 14.5) * Math.PI; right = (Math.Cos(a), Math.Sin(a)); }
		return held;
	}

	public void OpenPadSetup()
	{
		if (_padHost.Children.Count == 0) _padHost.Children.Add(PadSheet());
		_padGames = new() { (null, L.T("Todos los juegos")) };
		foreach (var game in _games)
		{
			var key = PadMap.GameKey(game.Name);
			if (!_padGames.Any(g => g.Key == key)) _padGames.Add((key, PadMap.DisplayName(game.Name)));
		}
		// It opens on the game chosen on the home screen.
		var chosen = _index < _games.Count ? PadMap.GameKey(_games[_index].Name) : null;
		_padGame = Math.Max(0, _padGames.FindIndex(g => g.Key == chosen));
		_padMap = PadMap.Current(PadGameKey);
		_padRow = 0;
		_padCapturing = false;
		BuildPadRows();
		_padHost.IsVisible = true;
		_demoStart = DateTime.Now;
		_padTimer ??= new DispatcherTimer(TimeSpan.FromMilliseconds(40), DispatcherPriority.Background, (_, _) => UpdatePadDrawing());
		_padTimer.Start();
		UpdatePadDrawing();
	}

	private void ClosePadSetup()
	{
		_padCapturing = false;
		if (Pad is not null) Pad.Capture = false;
		_padTimer?.Stop();
		_padHost.IsVisible = false;
		RefreshSettings();
	}

	private Border PadSheet()
	{
		var body = new StackPanel { Spacing = 18 };
		var head = new DockPanel { LastChildFill = false };
		head.Children.Add(Label(L.T("Configurar mando"), 40, FontWeight.Bold, TextColor));
		var close = Hint("B", L.T("Volver"));
		close.Cursor = new Cursor(StandardCursorType.Hand);
		close.PointerPressed += (_, _) => { if (_padCapturing) CancelCapture(); else ClosePadSetup(); };
		DockPanel.SetDock(close, Dock.Right);
		head.Children.Add(close);
		body.Children.Add(head);
		// The game: LB / RB (or a click) go to the previous or next one.
		var game = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 16, Cursor = new Cursor(StandardCursorType.Hand) };
		game.Children.Add(Label(L.T("Juego:"), 26, FontWeight.Normal, Dim));
		game.Children.Add(Label("◀", 26, FontWeight.Bold, Accent));
		game.Children.Add(_padGameLine);
		game.Children.Add(Label("▶", 26, FontWeight.Bold, Accent));
		game.Children.Add(Label(L.T("LB / RB cambian de juego"), 20, FontWeight.Normal, Dim));
		game.PointerPressed += (_, _) => ChangePadGame(1);
		body.Children.Add(game);
		body.Children.Add(_padGameNote);
		body.Children.Add(_padStatusLine);
		var content = new Grid { ColumnDefinitions = new ColumnDefinitions("780,*") };
		var drawing = PadDrawing();
		content.Children.Add(drawing);
		Grid.SetColumn(_padRows, 1);
		content.Children.Add(_padRows);
		body.Children.Add(content);
		return new Border
		{
			Width = 1760, Padding = new Thickness(44, 36), CornerRadius = new CornerRadius(28), Child = body,
			Background = new SolidColorBrush(Panel), BorderBrush = new SolidColorBrush(Line), BorderThickness = new Thickness(1),
			HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center,
		};
	}

	// ---- The drawing: a white modern pad drawn by NovaS (PadArt), with a light over each button --------------------
	// LB, RB, LT and RT do not show from the front: they are pills above it. Places as in Gamepad.PadButtons.

	private const double PhotoScale = 740.0 / 1100, PhotoLeft = 10, PhotoTop = 96;

	// Centre and radius in the photo's own pixels (PadArt's 1100 x 850 drawing).
	private static readonly Dictionary<string, (double X, double Y, double R)> PhotoSpots = new()
	{
		["ls"] = (270, 231, 62), ["rs"] = (689, 388, 62), ["back"] = (465, 222, 24), ["start"] = (625, 222, 24),
		["up"] = (406, 351, 26), ["down"] = (406, 461, 26), ["left"] = (351, 406, 26), ["right"] = (461, 406, 26),
		["west"] = (751, 222, 36), ["north"] = (824, 161, 36), ["east"] = (897, 230, 36), ["south"] = (824, 299, 36),
	};

	private Canvas PadDrawing()
	{
		var canvas = new Canvas { Width = 760, Height = 760 };
		void Add(Control c, double x, double y) { Canvas.SetLeft(c, x); Canvas.SetTop(c, y); canvas.Children.Add(c); }

		Add(PadArt(), PhotoLeft, PhotoTop);

		// Shoulder buttons and triggers, as pills above the photo.
		Pill("lt", "LT", 40); Pill("lb", "LB", 190); Pill("rb", "RB", 430); Pill("rt", "RT", 580);

		foreach (var (place, (px, py, pr)) in PhotoSpots)
		{
			var r = pr * PhotoScale + 4;
			var x = PhotoLeft + px * PhotoScale;
			var y = PhotoTop + py * PhotoScale;
			var spot = new Ellipse { Width = r * 2, Height = r * 2 };
			Spot(place, spot, x - r, y - r);
			// What it does (the game's action, or the Switch button), on a dark badge so it reads over the white pad.
			// Tags on the left of a button grow to the left, the rest to the right.
			var tag = Label("", 18, FontWeight.Bold, Accent);
			var badge = new Border { Child = tag, Background = new SolidColorBrush(Color.Parse("#1E2029")), CornerRadius = new CornerRadius(8), Padding = new Thickness(7, 1), IsHitTestVisible = false };
			var (tx, ty, toLeft) = place switch
			{
				"ls" => (x - r - 4, y - 14, true), "rs" => (x + r + 6, y + r - 26, false),
				"back" => (x + 8, y - r - 32, true), "start" => (x - 14, y + r + 6, false),
				"up" => (x + r + 2, y - r - 10, false), "down" => (x + r + 2, y, false), "left" => (x - r + 2, y + r - 4, true), "right" => (x + r + 4, y - 14, false),
				"north" => (x + r + 4, y - r - 10, false), "east" => (x + r + 4, y - 14, false), "south" => (x + r + 2, y + 4, false),
				_ => (x - r + 10, y - r - 28, true),
			};
			var holder = new Grid { Width = 240, IsHitTestVisible = false };
			badge.HorizontalAlignment = toLeft ? HorizontalAlignment.Right : HorizontalAlignment.Left;
			holder.Children.Add(badge);
			Add(holder, toLeft ? tx - 240 : tx, ty);
			_padTags[place] = tag;
		}
		_leftKnob = StickDot();
		_rightKnob = StickDot();
		return canvas;

		void Pill(string place, string text, double x)
		{
			var pill = new Rectangle { Width = 140, Height = 46, RadiusX = 23, RadiusY = 23 };
			Spot(place, pill, x, 40);
			pill.Fill = new SolidColorBrush(Knob);
			var label = Label(text, 22, FontWeight.Bold, TextColor, center: true);
			label.Width = 140; label.TextAlignment = TextAlignment.Center; label.IsHitTestVisible = false;
			Add(label, x, 48);
			var tag = Label("", 19, FontWeight.Bold, Accent, center: true);
			tag.Width = 180; tag.TextAlignment = TextAlignment.Center; tag.IsHitTestVisible = false;
			Add(tag, x - 20, 8);
			_padTags[place] = tag;
		}

		void Spot(string place, Shape shape, double x, double y)
		{
			shape.Fill = new SolidColorBrush(Colors.Transparent);
			shape.Stroke = new SolidColorBrush(Colors.Transparent);
			shape.StrokeThickness = 5;
			shape.Cursor = new Cursor(StandardCursorType.Hand);
			shape.PointerPressed += (_, _) => FocusPadPlace(place);
			Add(shape, x, y);
			_padParts[place] = shape;
		}

		// A dot on the stick's cap that follows where the stick points.
		Ellipse StickDot()
		{
			var knob = new Ellipse { Width = 22, Height = 22, Fill = new SolidColorBrush(Accent), Stroke = new SolidColorBrush(Colors.White), StrokeThickness = 3, IsHitTestVisible = false };
			canvas.Children.Add(knob);
			return knob;
		}
	}

	// The pad, drawn: a white modern-pad shape in the same 1100 x 850 space as PhotoSpots, with the face buttons'
	// letters centred in each button. No brand marks: the centre button is a plain light ring.
	private readonly Dictionary<string, TextBlock> _padFaces = new();

	private Control PadArt()
	{
		var art = new Canvas { Width = 1100, Height = 850 };
		void Put(Control c, double x, double y) { Canvas.SetLeft(c, x); Canvas.SetTop(c, y); art.Children.Add(c); }
		Ellipse Circle(double cx, double cy, double r, IBrush fill, IBrush? stroke = null, double thickness = 0)
		{
			var e = new Ellipse { Width = r * 2, Height = r * 2, Fill = fill, Stroke = stroke, StrokeThickness = thickness };
			Put(e, cx - r, cy - r);
			return e;
		}
		static IBrush Vertical(string top, string bottom) => new LinearGradientBrush
		{
			StartPoint = new RelativePoint(0.5, 0, RelativeUnit.Relative), EndPoint = new RelativePoint(0.5, 1, RelativeUnit.Relative),
			GradientStops = { new GradientStop(Color.Parse(top), 0), new GradientStop(Color.Parse(bottom), 1) },
		};
		var outline = new SolidColorBrush(Color.Parse("#B9BDC9"));

		// Bumpers peeking over the top, then the body.
		art.Children.Add(new Avalonia.Controls.Shapes.Path { Data = Geometry.Parse("M 175,95 C 190,40 260,22 360,26 L 420,30 L 400,75 Z"), Fill = Vertical("#E9EBF0", "#CFD3DC"), Stroke = outline, StrokeThickness = 2 });
		art.Children.Add(new Avalonia.Controls.Shapes.Path { Data = Geometry.Parse("M 925,95 C 910,40 840,22 740,26 L 680,30 L 700,75 Z"), Fill = Vertical("#E9EBF0", "#CFD3DC"), Stroke = outline, StrokeThickness = 2 });
		art.Children.Add(new Avalonia.Controls.Shapes.Path
		{
			Data = Geometry.Parse(
				"M 250,52 C 400,40 700,40 850,52 C 965,60 1015,125 1043,260 C 1078,410 1096,600 1086,705 " +
				"C 1080,765 1040,790 998,780 C 955,770 905,708 846,640 C 796,582 762,562 700,560 L 400,560 " +
				"C 338,562 304,582 254,640 C 195,708 145,770 102,780 C 60,790 20,765 14,705 " +
				"C 4,600 22,410 57,260 C 85,125 135,60 250,52 Z"),
			Fill = Vertical("#FFFFFF", "#E3E6EC"), Stroke = outline, StrokeThickness = 3,
		});
		// A soft inner line along the grips, for depth.
		art.Children.Add(new Avalonia.Controls.Shapes.Path { Data = Geometry.Parse("M 300,600 C 230,680 170,745 115,755"), Stroke = new SolidColorBrush(Color.Parse("#D3D6DE")), StrokeThickness = 4 });
		art.Children.Add(new Avalonia.Controls.Shapes.Path { Data = Geometry.Parse("M 800,600 C 870,680 930,745 985,755"), Stroke = new SolidColorBrush(Color.Parse("#D3D6DE")), StrokeThickness = 4 });

		// Centre button (plain), view, menu and share.
		Circle(550, 112, 44, Vertical("#2C2F38", "#15171C"));
		Circle(550, 112, 30, Brushes.Transparent, new SolidColorBrush(Color.Parse("#E8EAF0")), 5);
		foreach (var (place, glyph) in new[] { ("back", "⧉"), ("start", "≡") })
		{
			var (x, y, r) = PhotoSpots[place];
			Circle(x, y, r, Vertical("#FFFFFF", "#E2E5EB"), outline, 2);
			var g = Label(glyph, 24, FontWeight.Bold, Color.Parse("#4A4E5C"), center: true);
			g.Width = r * 2; g.TextAlignment = TextAlignment.Center;
			Put(g, x - r, y - 16);
		}
		var share = new Border { Width = 70, Height = 36, CornerRadius = new CornerRadius(18), Background = Vertical("#FFFFFF", "#E2E5EB"), BorderBrush = outline, BorderThickness = new Thickness(2) };
		Put(share, 515, 268);

		// Sticks: a white collar, a dark cap with a ridge.
		foreach (var place in new[] { "ls", "rs" })
		{
			var (x, y, r) = PhotoSpots[place];
			Circle(x, y, r + 16, Vertical("#FFFFFF", "#DADDE4"), outline, 2);
			Circle(x, y, r, Vertical("#3A3D46", "#16181D"));
			Circle(x, y, r - 14, Vertical("#2A2D35", "#3B3F48"));
		}

		// D-pad: a dark round base with a cross.
		{
			var (ux, uy, _) = PhotoSpots["up"];
			var (dx, dy, _) = PhotoSpots["down"];
			var cx = ux; var cy = (uy + dy) / 2;
			Circle(cx, cy, 84, Vertical("#2C2F38", "#121318"));
			var cross = Vertical("#4A4E58", "#2A2D34");
			Put(new Rectangle { Width = 44, Height = 150, RadiusX = 8, RadiusY = 8, Fill = cross }, cx - 22, cy - 75);
			Put(new Rectangle { Width = 150, Height = 44, RadiusX = 8, RadiusY = 8, Fill = cross }, cx - 75, cy - 22);
		}

		// Face buttons: dark domes with the letter in the middle, in its colour.
		foreach (var (place, color) in new[] { ("south", "#5BD45B"), ("east", "#F0505A"), ("west", "#3AA8F5"), ("north", "#F5D33A") })
		{
			var (x, y, r) = PhotoSpots[place];
			var face = new TextBlock
			{
				Text = place switch { "south" => "A", "east" => "B", "west" => "X", _ => "Y" },
				FontSize = 44, FontWeight = FontWeight.Bold, Foreground = new SolidColorBrush(Color.Parse(color)),
				HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center,
				TextAlignment = TextAlignment.Center,
			};
			var dome = new Border
			{
				Width = r * 2, Height = r * 2, CornerRadius = new CornerRadius(r), Child = face,
				Background = new RadialGradientBrush
				{
					GradientOrigin = new RelativePoint(0.35, 0.3, RelativeUnit.Relative), Center = new RelativePoint(0.5, 0.5, RelativeUnit.Relative),
					RadiusX = new RelativeScalar(0.7, RelativeUnit.Relative), RadiusY = new RelativeScalar(0.7, RelativeUnit.Relative),
					GradientStops = { new GradientStop(Color.Parse("#3A3D46"), 0), new GradientStop(Color.Parse("#0E0F12"), 1) },
				},
				BorderBrush = new SolidColorBrush(Color.Parse("#0A0B0D")), BorderThickness = new Thickness(2),
			};
			Put(dome, x - r, y - r);
			_padFaces[place] = face;
		}
		return new Viewbox { Width = 1100 * PhotoScale, Height = 850 * PhotoScale, Stretch = Stretch.Uniform, Child = art };
	}

	private void UpdatePadDrawing()
	{
		if (!PadSetupOpen) return;
		(double X, double Y) left = default, right = default;
		var held = DemoPad ? DemoHeld(out left, out right) : Pad is not null ? Pad.Held(out left, out right) : new HashSet<string>();
		var focused = _padRow < PadButtonRows ? _padMap[PadMap.Buttons[_padRow].Id] : null;
		foreach (var (place, shape) in _padParts)
		{
			var down = held.Contains(place);
			var pill = place is "lt" or "lb" or "rb" or "rt";
			shape.Fill = new SolidColorBrush(down ? Color.FromArgb(150, Accent.R, Accent.G, Accent.B) : pill ? Knob : Colors.Transparent);
			shape.Stroke = new SolidColorBrush(place == focused ? Red : down ? Accent : pill ? Color.Parse("#4A4E5C") : Colors.Transparent);
			shape.StrokeThickness = place == focused ? 6 : down ? 4 : 2;
		}
		PlaceKnob(_leftKnob, "ls", left);
		PlaceKnob(_rightKnob, "rs", right);
		foreach (var (place, face) in _padFaces) face.Text = PadMap.PadName(place, Pad);
		_padStatusLine.Text = PadPresent
		    ? (_padCapturing ? L.F("Pulsa en tu mando el botón que quieres para «{0}»…  (B en el teclado o Esc cancela)", PadMap.Buttons[_padRow].Name)
		                     : L.T("Pulsa los botones de tu mando: se iluminan en el dibujo. En azul, el botón de Switch que hace cada uno."))
		    : L.T("Conecta un mando para ver sus botones y cambiarlos.");

		void PlaceKnob(Ellipse? knob, string place, (double X, double Y) stick)
		{
			if (knob is null) return;
			var (px, py, pr) = PhotoSpots[place];
			var x = PhotoLeft + px * PhotoScale + stick.X * pr * PhotoScale * 0.55;
			var y = PhotoTop + py * PhotoScale + stick.Y * pr * PhotoScale * 0.55;
			Canvas.SetLeft(knob, x - 11);
			Canvas.SetTop(knob, y - 11);
		}
	}

	// ---- The list: the Switch's buttons in two columns, then the two styles and Listo --------------------------

	private void BuildPadRows()
	{
		_padRows.Children.Clear();
		var gameActions = PadMap.Actions(PadGameKey);
		foreach (var (place, tag) in _padTags)
		{
			var does = PadMap.Buttons.Where(b => _padMap[b.Id] == place).ToList();
			tag.Text = does.Count == 0 ? ""
			    : gameActions is not null && does.All(b => gameActions.ContainsKey(b.Id)) ? string.Join(", ", does.Select(b => gameActions[b.Id]))
			    : "→ " + string.Join(" ", does.Select(b => PadMap.ShortName(b.Id)));
			if (tag.Parent is Border badge) badge.IsVisible = tag.Text.Length > 0;
		}
		_padGameLine.Text = _padGames.Count > 0 ? _padGames[_padGame].Name : L.T("Todos los juegos");
		_padGameNote.Text = PadGameKey is null
		    ? L.T("Lo que elijas aquí vale para todos los juegos que no tengan su propia configuración.")
		    : PadMap.Custom(PadGameKey) is null
		        ? L.T("Este juego usa la configuración de «Todos los juegos». Si cambias algo, se guarda solo para él.")
		        : L.T("Este juego tiene su propia configuración.");
		var columns = new Grid { ColumnDefinitions = new ColumnDefinitions("*,*") };
		var half = (PadButtonRows + 1) / 2;
		for (var column = 0; column < 2; column++)
		{
			var stack = new StackPanel { Spacing = 8, Margin = new Thickness(column == 0 ? 0 : 7, 0, column == 0 ? 7 : 0, 0) };
			for (var i = column * half; i < Math.Min(PadButtonRows, (column + 1) * half); i++)
				stack.Children.Add(PadButtonRow(i));
			Grid.SetColumn(stack, column);
			columns.Children.Add(stack);
		}
		_padRows.Children.Add(Label(gameActions is null ? L.T("Botón de Switch  →  botón de tu mando") : L.T("Botón de Switch: lo que hace en el juego  →  botón de tu mando"), 20, FontWeight.Normal, Dim));
		_padRows.Children.Add(columns);
		var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 14, Margin = new Thickness(0, 10, 0, 0) };
		var names = PadGameKey is null
		    ? new[] { L.T("Tipo Switch"), L.T("Tipo Xbox"), L.T("Listo") }
		    : new[] { L.T("Tipo Switch"), L.T("Tipo Xbox"), L.T("Como en todos"), L.T("Listo") };
		for (var i = 0; i < PadActions; i++)
		{
			var index = PadButtonRows + i;
			var focused = _padRow == index;
			var button = new Border
			{
				Padding = new Thickness(26, 14), CornerRadius = new CornerRadius(14), Cursor = new Cursor(StandardCursorType.Hand),
				Background = new SolidColorBrush(focused ? Color.Parse("#2F3340") : Back),
				BorderBrush = new SolidColorBrush(focused ? Accent : Line), BorderThickness = new Thickness(3),
				Child = Label(names[i], 24, FontWeight.SemiBold, focused ? Accent : TextColor),
			};
			button.PointerPressed += (_, _) => { _padRow = index; PadAccept(); };
			actions.Children.Add(button);
		}
		_padRows.Children.Add(actions);
		_padRows.Children.Add(new TextBlock
		{
			Text = L.T("Tipo Switch: cada botón en el mismo sitio que en la Switch. Tipo Xbox: la letra de tu mando es la de Switch (en Donkey Kong Bananza saltas con A, como en un juego de Xbox). Se aplica al abrir el juego."),
			FontSize = 18, TextWrapping = TextWrapping.Wrap, Foreground = new SolidColorBrush(Dim), MaxWidth = 860, HorizontalAlignment = HorizontalAlignment.Left,
		});
		UpdatePadDrawing();
	}

	private Border PadButtonRow(int index)
	{
		var button = PadMap.Buttons[index];
		var focused = _padRow == index;
		var row = new DockPanel { LastChildFill = true };
		var value = focused && _padCapturing
		    ? Label(L.T("Pulsa un botón…"), 24, FontWeight.SemiBold, Red)
		    : Label(PadMap.PadName(_padMap[button.Id], Pad), 24, FontWeight.SemiBold, focused ? Accent : TextColor);
		DockPanel.SetDock(value, Dock.Right);
		row.Children.Add(value);
		// "A: Saltar": the Switch button and, in a game NovaS knows, what it does there.
		var name = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
		var action = PadMap.Actions(PadGameKey)?.GetValueOrDefault(button.Id);
		name.Children.Add(Label(action is null ? button.Name : button.Name + ":", 26, FontWeight.Bold, TextColor));
		if (action is not null) name.Children.Add(Label(action, 21, FontWeight.SemiBold, Color.Parse("#7FDCF5")));
		row.Children.Add(name);
		var box = new Border
		{
			Child = row, Padding = new Thickness(22, 10), CornerRadius = new CornerRadius(14), Cursor = new Cursor(StandardCursorType.Hand),
			Background = new SolidColorBrush(focused ? Color.Parse("#2F3340") : Back),
			BorderBrush = new SolidColorBrush(focused ? (_padCapturing ? Red : Accent) : Back), BorderThickness = new Thickness(3),
		};
		box.PointerPressed += (_, _) => { _padRow = index; PadAccept(); };
		return box;
	}

	private void FocusPadPlace(string place)
	{
		if (_padCapturing) return;
		var index = Array.FindIndex(PadMap.Buttons, b => _padMap[b.Id] == place);
		if (index >= 0) { _padRow = index; BuildPadRows(); }
	}

	private void PadAccept()
	{
		if (_padRow < PadButtonRows)
		{
			if (!PadPresent)
			{
				Toast(L.T("Conecta un mando para cambiar sus botones"));
				return;
			}
			_padCapturing = true;
			if (Pad is not null) Pad.Capture = true;
			BuildPadRows();
			if (DemoPad)
				DispatcherTimer.RunOnce(() =>
				{
					_demoPress = (DateTime.Now.AddMilliseconds(500), "north");
					PadRaw("north");
				}, TimeSpan.FromSeconds(2.2));
			return;
		}
		switch (_padRow - PadButtonRows)
		{
			case 0: UseStyle(false); break;
			case 1: UseStyle(true); break;
			case 2 when PadGameKey is not null: SameAsAll(); break;
			default: ClosePadSetup(); break;
		}
	}

	// For all games: the style of Botones del mando. For one game: its own map in that style.
	private void UseStyle(bool byLabel)
	{
		try
		{
			if (PadGameKey is null) PadMap.UseStyle(byLabel);
			else PadMap.Save(new(byLabel ? PadMap.ByLabel : PadMap.ByPosition), PadGameKey);
		}
		catch (Exception e) when (e is IOException or UnauthorizedAccessException)
		{
			Toast(L.T("No se pudo guardar: ") + e.Message);
		}
		_padMap = PadMap.Current(PadGameKey);
		BuildPadRows();
		Toast(byLabel ? L.T("Tipo Xbox: la letra de tu mando es la de Switch") : L.T("Tipo Switch: cada botón donde está en la Switch"));
	}

	// One game back to the configuration of «Todos los juegos».
	private void SameAsAll()
	{
		try
		{
			PadMap.Forget(PadGameKey);
		}
		catch (Exception e) when (e is IOException or UnauthorizedAccessException)
		{
			Toast(L.T("No se pudo guardar: ") + e.Message);
		}
		_padMap = PadMap.Current(PadGameKey);
		BuildPadRows();
		Toast(L.T("Este juego usa ahora la configuración de «Todos los juegos»"));
	}

	private void ChangePadGame(int step)
	{
		if (_padCapturing || _padGames.Count == 0) return;
		_padGame = (_padGame + step + _padGames.Count) % _padGames.Count;
		_padMap = PadMap.Current(PadGameKey);
		_padRow = Math.Min(_padRow, PadButtonRows + PadActions - 1);
		BuildPadRows();
	}

	private void CancelCapture()
	{
		_padCapturing = false;
		if (Pad is not null) Pad.Capture = false;
		BuildPadRows();
	}

	// A pad button while Configurar mando is open: when waiting for one, it goes to the chosen Switch button; the
	// Switch button that had it takes the old one, so no button is left without its pad button.
	public void PadRaw(string place)
	{
		if (!PadSetupOpen || !_padCapturing) return;
		var target = PadMap.Buttons[_padRow].Id;
		var old = _padMap[target];
		foreach (var b in PadMap.Buttons)
			if (b.Id != target && _padMap[b.Id] == place) _padMap[b.Id] = old;
		_padMap[target] = place;
		try
		{
			PadMap.Save(_padMap, PadGameKey);
		}
		catch (Exception e) when (e is IOException or UnauthorizedAccessException)
		{
			Toast(L.T("No se pudo guardar: ") + e.Message);
		}
		Toast(L.F("«{0}» ahora es {1} en tu mando", PadMap.Buttons[_padRow].Name, PadMap.PadName(place, Pad)));
		// Leave capture after this frame, so the same press does not also act on the menu.
		Dispatcher.UIThread.Post(() => { _padCapturing = false; if (Pad is not null) Pad.Capture = false; BuildPadRows(); }, DispatcherPriority.Background);
	}

	private void PadSetupInput(PadInput input)
	{
		if (_padCapturing)
		{
			if (input is PadInput.Back or PadInput.Menu or PadInput.Options) CancelCapture();
			return;
		}
		var half = (PadButtonRows + 1) / 2;
		switch (input)
		{
			case PadInput.Up:
				_padRow = _padRow >= PadButtonRows ? half - 1 : _padRow is 0 || _padRow == half ? _padRow : _padRow - 1;
				break;
			case PadInput.Down:
				_padRow = _padRow >= PadButtonRows ? _padRow : _padRow == half - 1 || _padRow == PadButtonRows - 1 ? PadButtonRows : _padRow + 1;
				break;
			case PadInput.Left:
				_padRow = _padRow >= PadButtonRows ? Math.Max(PadButtonRows, _padRow - 1) : _padRow >= half ? _padRow - half : _padRow;
				break;
			case PadInput.Right:
				_padRow = _padRow >= PadButtonRows ? Math.Min(PadButtonRows + PadActions - 1, _padRow + 1) : _padRow < half ? Math.Min(_padRow + half, PadButtonRows - 1) : _padRow;
				break;
			case PadInput.Accept: PadAccept(); return;
			case PadInput.PrevTab: ChangePadGame(-1); return;
			case PadInput.NextTab: ChangePadGame(1); return;
			case PadInput.Back or PadInput.Options or PadInput.Menu: ClosePadSetup(); return;
		}
		BuildPadRows();
	}
}
