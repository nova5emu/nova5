using Avalonia;
using Avalonia.Animation;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Controls.Shapes;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Threading;
using Nova5.Services;

namespace Nova5;

// NovaS home: the Switch 2 side of Nova5, laid out like a console home screen. A row of large square games with the
// focused one outlined and its name above, a row of round buttons under it, the clock at the top and the controls at
// the bottom. It draws on a 1920x1080 stage like the rest of the launcher.
public sealed class NovaSHome : Grid
{
	private const double Tile = 300, Gap = 26, RowLeft = 96, RowTop = 250;

	private static readonly Color Back      = Color.Parse("#16171C");
	private static readonly Color Panel     = Color.Parse("#24262E");
	private static readonly Color Line      = Color.Parse("#3A3D48");
	private static readonly Color Accent    = Color.Parse("#2EC5F0");
	private static readonly Color TextColor = Color.Parse("#F2F3F7");
	private static readonly Color Dim       = Color.Parse("#9A9EAD");
	private static readonly FontFamily Icons = new("Segoe Fluent Icons, Segoe MDL2 Assets");

	private sealed record Action(string Glyph, string Label, System.Action Run);

	private readonly StackPanel _row = new() { Orientation = Orientation.Horizontal, Spacing = Gap };
	private readonly TranslateTransform _shift = new();
	private readonly TextBlock _name = Label("", 40, FontWeight.SemiBold, TextColor);
	private readonly TextBlock _detail = Label("", 22, FontWeight.Normal, Dim);
	private readonly StackPanel _actions = new() { Orientation = Orientation.Horizontal, Spacing = 34, HorizontalAlignment = HorizontalAlignment.Center };
	private readonly TextBlock _clock = Label("", 30, FontWeight.SemiBold, TextColor);
	private readonly TextBlock _pad = Label("", 20, FontWeight.Normal, Dim);
	private readonly Border _banner = new() { IsVisible = false };
	private readonly TextBlock _bannerText = new() { FontSize = 22, TextWrapping = TextWrapping.Wrap, Foreground = new SolidColorBrush(TextColor) };
	private readonly List<Action> _buttons;
	private readonly TextBlock _toastText = new() { FontSize = 24, Foreground = new SolidColorBrush(TextColor) };
	private readonly Border _toast = new()
	{
		Padding = new Thickness(28, 14), CornerRadius = new CornerRadius(16), Opacity = 0, HorizontalAlignment = HorizontalAlignment.Center,
		Background = new SolidColorBrush(Color.Parse("#2C2F38")), BorderBrush = new SolidColorBrush(Line), BorderThickness = new Thickness(1),
		Transitions = new Transitions { new DoubleTransition { Property = OpacityProperty, Duration = TimeSpan.FromMilliseconds(220) } },
	};
	private IDisposable? _toastHide;

	private List<SwitchGame> _games = new();
	private int _index, _button;
	private bool _onButtons;

	public event System.Action<SwitchGame>? Launch;
	public event System.Action? Leave;           // Back to the PS5 side.
	public event System.Action? AddFolder;
	public event System.Action? InstallKeys;     // Pick prod.keys / title.keys.
	public event System.Action? InstallFirmware; // Pick a firmware .zip or any .nca of its folder.
	public event System.Action<SwitchGame>? ChangeCover; // Pick an image as a game's cover.

	public NovaSHome()
	{
		IsVisible  = false;
		Background = new SolidColorBrush(Back);
		_buttons = new List<Action>
		{
			new("", "Añadir carpeta", () => AddFolder?.Invoke()),
			new("", "Claves", () => InstallKeys?.Invoke()),
			new("", "Firmware", () => InstallFirmware?.Invoke()),
			new("", "Portada", () => { if (_index < _games.Count) ChangeCover?.Invoke(_games[_index]); else Toast("Elige primero un juego"); }),
			new("", "Ajustes NovaS", OpenSettings),
			// NovaS on its own has no PS5 side to go back to: its last button closes it.
			Edition.NovaSOnly ? new("", "Salir", () => Leave?.Invoke()) : new("", "Nova5 (PS5)", () => Leave?.Invoke()),
		};

		var stage = new Canvas { Width = 1920, Height = 1080 };

		// Top: player and brand on the left, clock and controller on the right.
		var who = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 18 };
		who.Children.Add(new Border
		{
			Width = 62, Height = 62, CornerRadius = new CornerRadius(31), BorderThickness = new Thickness(3),
			BorderBrush = new SolidColorBrush(Accent), Background = new SolidColorBrush(Panel),
			Child = Label("", 28, FontWeight.Normal, TextColor, Icons, center: true),
		});
		var brand = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 2, VerticalAlignment = VerticalAlignment.Center };
		brand.Children.Add(Label("Nova", 34, FontWeight.Bold, TextColor));
		brand.Children.Add(Label("S", 34, FontWeight.Black, Accent));
		who.Children.Add(brand);
		Place(stage, who, RowLeft, 54);

		var status = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 30, VerticalAlignment = VerticalAlignment.Center };
		status.Children.Add(_pad);
		status.Children.Add(_clock);
		var statusDock = new DockPanel { Width = 1920 - RowLeft * 2, LastChildFill = false };
		DockPanel.SetDock(status, Dock.Right);
		statusDock.Children.Add(status);
		Place(stage, statusDock, RowLeft, 66);

		// The focused game's name, then the row of games.
		var info = new StackPanel { Spacing = 4 };
		info.Children.Add(_name);
		info.Children.Add(_detail);
		Place(stage, info, RowLeft, 150);
		var rowHost = new Canvas { Width = 1920 - RowLeft, Height = Tile + 40 };
		_row.RenderTransform = _shift;
		_shift.Transitions = new Transitions { new DoubleTransition { Property = TranslateTransform.XProperty, Duration = TimeSpan.FromMilliseconds(200), Easing = new Avalonia.Animation.Easings.CubicEaseOut() } };
		rowHost.Children.Add(_row);
		Place(stage, rowHost, RowLeft, RowTop);

		// Missing keys or engine: what to do, above the buttons.
		_banner.Child = _bannerText;
		_banner.Width = 1920 - RowLeft * 2;
		_banner.Padding = new Thickness(28, 18);
		_banner.CornerRadius = new CornerRadius(18);
		_banner.Background = new SolidColorBrush(Panel);
		_banner.BorderBrush = new SolidColorBrush(Accent, 0.6);
		_banner.BorderThickness = new Thickness(1);
		Place(stage, _banner, RowLeft, RowTop + Tile + 50);

		// Round buttons, centered.
		BuildActions();
		var actionsDock = new Panel { Width = 1920 };
		actionsDock.Children.Add(_actions);
		Place(stage, actionsDock, 0, 780);

		// Bottom bar: a thin line, the controls on the right.
		Place(stage, new Rectangle { Width = 1920 - RowLeft * 2, Height = 1, Fill = new SolidColorBrush(Line) }, RowLeft, 980);
		var hints = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 34 };
		hints.Children.Add(Hint("A", "Iniciar"));
		hints.Children.Add(Hint("Y", "Ajustes"));
		if (!Edition.NovaSOnly) hints.Children.Add(Hint("B", "Nova5"));
		var hintsDock = new DockPanel { Width = 1920 - RowLeft * 2, LastChildFill = false };
		var console = Label("Juegos de Switch 2", 20, FontWeight.Normal, Dim);
		DockPanel.SetDock(console, Dock.Left);
		DockPanel.SetDock(hints, Dock.Right);
		hintsDock.Children.Add(console);
		hintsDock.Children.Add(hints);
		Place(stage, hintsDock, RowLeft, 1000);

		_toast.Child = _toastText;
		var toastDock = new Panel { Width = 1920 };
		toastDock.Children.Add(_toast);
		Place(stage, toastDock, 0, 680);

		Children.Add(new Viewbox { Stretch = Stretch.Uniform, Child = stage });

		var clock = new DispatcherTimer(TimeSpan.FromSeconds(1), DispatcherPriority.Background, (_, _) => _clock.Text = DateTime.Now.ToString("HH:mm"));
		clock.Start();
		_clock.Text = DateTime.Now.ToString("HH:mm");
	}

	public string PadStatus { set => _pad.Text = value; }

	private void BuildActions()
	{
		_actions.Children.Clear();
		for (var i = 0; i < _buttons.Count; i++)
		{
			var index = i;
			var round = RoundButton(_buttons[i]);
			round.PointerPressed += (_, _) => { _onButtons = true; _button = index; Refresh(); _buttons[index].Run(); };
			_actions.Children.Add(round);
		}
	}

	// A newer version was found (NovaS on its own): one more round button, first in the row, that installs it.
	public void OfferUpdate(string label, System.Action run)
	{
		_buttons.RemoveAll(b => b.Glyph == UpdateGlyph);
		_buttons.Insert(0, new(UpdateGlyph, label, run));
		BuildActions();
		Refresh();
	}

	private const string UpdateGlyph = "";

	public void Toast(string message)
	{
		_toastText.Text = message;
		_toast.Opacity  = 1;
		_toastHide?.Dispose();
		_toastHide = DispatcherTimer.RunOnce(() => _toast.Opacity = 0, TimeSpan.FromSeconds(3.5));
	}

	public void Load(List<SwitchGame> games)
	{
		var previous = _games.ElementAtOrDefault(_index)?.Path;
		_games = games;
		_index = Math.Max(0, _games.FindIndex(g => g.Path == previous));
		_row.Children.Clear();
		for (var i = 0; i < _games.Count; i++)
		{
			var index = i;
			var tile = GameTile(_games[i]);
			tile.PointerPressed += (sender, e) =>
			{
				// Right click: choose this game's cover.
				if (e.GetCurrentPoint(sender as Control).Properties.IsRightButtonPressed)
				{
					ChangeCover?.Invoke(_games[index]);
					return;
				}
				if (_onButtons || index != _index) { _onButtons = false; _index = index; Refresh(); }
				else Accept();
			};
			_row.Children.Add(tile);
		}
		var add = EmptyTile();
		add.PointerPressed += (_, _) => AddFolder?.Invoke();
		_row.Children.Add(add);
		Refresh();
	}

	// ---- Settings sheet (written to the engine's own ini, see SwitchGames.Options) -----------------

	private readonly StackPanel _settingsRows = new() { Spacing = 8 };
	// The options are taller than the screen: the mouse wheel scrolls them (it does not change values here).
	private readonly ScrollViewer _settingsScroll = new()
	{
		MaxHeight = 760, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
		VerticalScrollBarVisibility = ScrollBarVisibility.Auto, Padding = new Thickness(0, 0, 18, 0),
	};
	private Border? _settingsSheet;
	private int _setting;

	public void OpenSettings()
	{
		try { SwitchGames.ApplyDefaults(); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
		if (_settingsSheet is null)
		{
			var body = new StackPanel { Spacing = 22 };
			var head = new DockPanel { LastChildFill = false };
			head.Children.Add(Label("Ajustes de NovaS", 40, FontWeight.Bold, TextColor));
			var close = Hint("B", "Volver");
			close.Cursor = new Cursor(StandardCursorType.Hand);
			close.PointerPressed += (_, _) => CloseSettings();
			DockPanel.SetDock(close, Dock.Right);
			head.Children.Add(close);
			body.Children.Add(head);
			body.Children.Add(Label("▲ ▼ / rueda del ratón elegir y desplazar     ◂ ▸ / A / clic cambiar     B volver", 20, FontWeight.Normal, Dim));
			_settingsScroll.Content = _settingsRows;
			body.Children.Add(_settingsScroll);
			_settingsSheet = new Border
			{
				Width = 1240, Padding = new Thickness(48, 40), CornerRadius = new CornerRadius(28), Child = body,
				Background = new SolidColorBrush(Panel), BorderBrush = new SolidColorBrush(Line), BorderThickness = new Thickness(1),
				HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center,
			};
			var shade = new Grid { Background = new SolidColorBrush(Colors.Black, 0.55), Children = { _settingsSheet } };
			Children.Add(new Viewbox { Stretch = Stretch.Uniform, Child = new Grid { Width = 1920, Height = 1080, Children = { shade } } });
		}
		_setting = 0;
		BuildSettings();
		_settingsScroll.Offset = default;
		((Control)_settingsSheet.Parent!.Parent!.Parent!).IsVisible = true;
	}

	public bool SettingsOpen => _settingsSheet?.Parent?.Parent?.Parent is Control { IsVisible: true };

	private void CloseSettings()
	{
		if (_settingsSheet?.Parent?.Parent?.Parent is Control host) host.IsVisible = false;
	}

	private void BuildSettings()
	{
		_settingsRows.Children.Clear();
		for (var i = 0; i < SwitchGames.Options.Length; i++)
		{
			var index  = i;
			var option = SwitchGames.Options[i];
			var focused = i == _setting;
			var row = new DockPanel { LastChildFill = true };
			var value = Label("‹  " + option.Choices[SwitchGames.Current(option)].Name + "  ›", 24, FontWeight.SemiBold, focused ? Accent : TextColor);
			DockPanel.SetDock(value, Dock.Right);
			row.Children.Add(value);
			var text = new StackPanel { Spacing = 4, HorizontalAlignment = HorizontalAlignment.Left };
			text.Children.Add(Label(option.Label, 26, FontWeight.SemiBold, TextColor));
			text.Children.Add(new TextBlock { Text = option.Detail, FontSize = 18, TextWrapping = TextWrapping.Wrap, Foreground = new SolidColorBrush(Dim), MaxWidth = 760, HorizontalAlignment = HorizontalAlignment.Left });
			row.Children.Add(text);
			var box = new Border
			{
				Child = row, Padding = new Thickness(26, 16), CornerRadius = new CornerRadius(16), Cursor = new Cursor(StandardCursorType.Hand),
				Background = new SolidColorBrush(focused ? Color.Parse("#2F3340") : Back),
				BorderBrush = new SolidColorBrush(focused ? Accent : Back), BorderThickness = new Thickness(3),
			};
			box.PointerPressed += (_, _) => { _setting = index; Change(1); };
			_settingsRows.Children.Add(box);
		}
	}

	// Pad and keyboard move the selection; keep it on screen.
	private void SelectSetting(int index)
	{
		_setting = Math.Clamp(index, 0, SwitchGames.Options.Length - 1);
		BuildSettings();
		var row = (Control)_settingsRows.Children[_setting];
		Dispatcher.UIThread.Post(() => row.BringIntoView(), DispatcherPriority.Background);
	}

	private void Change(int step)
	{
		var option = SwitchGames.Options[_setting];
		var next = (SwitchGames.Current(option) + step + option.Choices.Length) % option.Choices.Length;
		try
		{
			SwitchGames.Set(option, next);
		}
		catch (Exception e) when (e is IOException or UnauthorizedAccessException)
		{
			Toast("No se pudo guardar: " + e.Message);
		}
		BuildSettings();
	}

	private void SettingsInput(PadInput input)
	{
		switch (input)
		{
			case PadInput.Up:   SelectSetting(_setting - 1); break;
			case PadInput.Down: SelectSetting(_setting + 1); break;
			case PadInput.Left: Change(-1); break;
			case PadInput.Right or PadInput.Accept: Change(1); break;
			case PadInput.Back or PadInput.Options or PadInput.Menu: CloseSettings(); break;
		}
	}

	// Pad and keyboard.
	public void Input(PadInput input)
	{
		if (SettingsOpen)
		{
			SettingsInput(input);
			return;
		}
		switch (input)
		{
			case PadInput.Left:  Move(-1); break;
			case PadInput.Right: Move(1); break;
			case PadInput.Down when !_onButtons: _onButtons = true; Refresh(); break;
			case PadInput.Up when _onButtons:    _onButtons = false; Refresh(); break;
			case PadInput.Accept: Accept(); break;
			case PadInput.Options or PadInput.Menu: OpenSettings(); break;
			case PadInput.Back when _onButtons: _onButtons = false; Refresh(); break;
			case PadInput.Back when !Edition.NovaSOnly: Leave?.Invoke(); break; // On its own, only "Salir" closes NovaS.
		}
	}

	private void Move(int step)
	{
		if (_onButtons) _button = Math.Clamp(_button + step, 0, _buttons.Count - 1);
		else _index = Math.Clamp(_index + step, 0, _games.Count); // The last tile adds a folder.
		Refresh();
	}

	private void Accept()
	{
		if (_onButtons) _buttons[_button].Run();
		else if (_index < _games.Count) Launch?.Invoke(_games[_index]);
		else AddFolder?.Invoke();
	}

	private void Refresh()
	{
		for (var i = 0; i < _row.Children.Count; i++)
		{
			var tile = (Border)_row.Children[i];
			var focused = !_onButtons && i == _index;
			tile.BorderBrush = focused ? new SolidColorBrush(Accent) : Brushes.Transparent;
			tile.RenderTransform = new ScaleTransform(focused ? 1.04 : 1, focused ? 1.04 : 1);
		}
		for (var i = 0; i < _actions.Children.Count; i++)
		{
			var circle = (Border)((StackPanel)_actions.Children[i]).Children[0];
			circle.BorderBrush = _onButtons && i == _button ? new SolidColorBrush(Accent) : new SolidColorBrush(Line);
		}
		const int visible = 5;
		_shift.X = -Math.Max(0, _index - (visible - 2)) * (Tile + Gap);

		if (_onButtons)
		{
			_name.Text   = _buttons[_button].Label;
			_detail.Text = "";
		}
		else if (_index < _games.Count)
		{
			var game = _games[_index];
			_name.Text   = game.Name;
			_detail.Text = game.Extracted
			    ? $"{(game.Path.EndsWith(".nup", StringComparison.OrdinalIgnoreCase) ? "Paquete NUP" : "Carpeta descifrada")} · no necesita claves · {game.Size / 1e9:0.0} GB"
			    : $"{System.IO.Path.GetExtension(game.Path).TrimStart('.').ToUpperInvariant()} · {game.Size / 1e9:0.0} GB";
		}
		else
		{
			_name.Text   = _games.Count == 0 ? "Añade tus juegos de Switch 2" : "Añadir carpeta";
			_detail.Text = "Elige la carpeta con tus .nsp, .xci, .nup o juegos descifrados (exefs + romfs)";
		}

		// Extracted (decrypted) games boot without keys; .nsp/.xci need the console's own.
		var needsKeys = _games.Count == 0 || _games.Any(g => !g.Extracted);
		var missingKeys = needsKeys && !SwitchGames.HasKeys;
		_banner.IsVisible = !SwitchGames.HasEngine || missingKeys || !SwitchGames.HasFirmware;
		_bannerText.Text = !SwitchGames.HasEngine
		    ? $"Falta el motor de NovaS. Actualiza {Edition.Name} para instalarlo."
		    : missingKeys
		    ? "Los .nsp y .xci necesitan las claves de TU consola: pulsa \"Claves\" y elige tu prod.keys (y title.keys si lo tienes). " +
		      "Los .nup y los juegos en carpeta descifrada (exefs + romfs) no las necesitan."
		    : "Recomendado: instala el firmware de tu consola con \"Firmware\" (elige su .zip o cualquier .nca de su carpeta). " +
		      "Muchos juegos arrancan sin él, pero algunos lo necesitan para fuentes, teclado y menús.";
	}

	// ---- Pieces -------------------------------------------------------------------------------

	private static Border GameTile(SwitchGame game)
	{
		// The game's own icon when an extracted game has one.
		if (game.Icon is { } icon)
		{
			try
			{
				return Square(new SolidColorBrush(Panel), new Image { Source = new Avalonia.Media.Imaging.Bitmap(icon), Stretch = Stretch.UniformToFill });
			}
			catch (Exception e) when (e is IOException or ArgumentException or NotSupportedException)
			{
			}
		}
		var hue = (uint)game.Name.Aggregate(17, (h, c) => unchecked(h * 31 + c)) % 360; // Stable: the same colour every time.
		var face = new Grid();
		face.Children.Add(new TextBlock
		{
			Text = Initials(game.Name), FontSize = 96, FontWeight = FontWeight.Black, Foreground = new SolidColorBrush(Colors.White, 0.92),
			HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center,
		});
		face.Children.Add(new TextBlock
		{
			Text = game.Name, FontSize = 20, FontWeight = FontWeight.SemiBold, Foreground = new SolidColorBrush(Colors.White, 0.9),
			TextWrapping = TextWrapping.Wrap, MaxLines = 2, Margin = new Thickness(18), VerticalAlignment = VerticalAlignment.Bottom,
		});
		return Square(new LinearGradientBrush
		{
			StartPoint = new RelativePoint(0, 0, RelativeUnit.Relative), EndPoint = new RelativePoint(1, 1, RelativeUnit.Relative),
			GradientStops = { new GradientStop(Hsv(hue, 0.65, 0.78), 0), new GradientStop(Hsv((hue + 40) % 360, 0.75, 0.42), 1) },
		}, face);
	}

	private static Border EmptyTile() =>
	    Square(new SolidColorBrush(Panel), Label("", 64, FontWeight.Normal, Dim, Icons, center: true));

	private static Border Square(IBrush fill, Control child) => new()
	{
		Width = Tile, Height = Tile, CornerRadius = new CornerRadius(22), Background = fill, Child = child, ClipToBounds = true,
		BorderThickness = new Thickness(6), BorderBrush = Brushes.Transparent, Cursor = new Cursor(StandardCursorType.Hand),
		RenderTransformOrigin = new RelativePoint(0.5, 0.5, RelativeUnit.Relative),
	};

	private static StackPanel RoundButton(Action action)
	{
		var panel = new StackPanel { Spacing = 10, Cursor = new Cursor(StandardCursorType.Hand) };
		panel.Children.Add(new Border
		{
			Width = 92, Height = 92, CornerRadius = new CornerRadius(46), Background = new SolidColorBrush(Panel),
			BorderThickness = new Thickness(4), BorderBrush = new SolidColorBrush(Line), HorizontalAlignment = HorizontalAlignment.Center,
			Child = Label(action.Glyph, 36, FontWeight.Normal, TextColor, Icons, center: true),
		});
		panel.Children.Add(Label(action.Label, 18, FontWeight.Normal, Dim, center: true));
		return panel;
	}

	private static StackPanel Hint(string key, string label)
	{
		var hint = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
		hint.Children.Add(new Border
		{
			Width = 34, Height = 34, CornerRadius = new CornerRadius(17), Background = new SolidColorBrush(TextColor),
			Child = Label(key, 18, FontWeight.Bold, Back, center: true),
		});
		hint.Children.Add(Label(label, 22, FontWeight.Normal, TextColor));
		return hint;
	}

	private static TextBlock Label(string text, double size, FontWeight weight, Color color, FontFamily? font = null, bool center = false)
	{
		var label = new TextBlock { Text = text, FontSize = size, FontWeight = weight, Foreground = new SolidColorBrush(color), VerticalAlignment = VerticalAlignment.Center };
		if (font is not null) label.FontFamily = font;
		if (center) label.HorizontalAlignment = HorizontalAlignment.Center;
		return label;
	}

	private static string Initials(string name)
	{
		var words = name.Split(' ', StringSplitOptions.RemoveEmptyEntries).Where(w => char.IsLetterOrDigit(w[0])).Take(2).ToList();
		return words.Count == 0 ? "?" : string.Concat(words.Select(w => char.ToUpperInvariant(w[0])));
	}

	private static Color Hsv(double h, double s, double v)
	{
		var c = v * s;
		var x = c * (1 - Math.Abs(h / 60 % 2 - 1));
		var m = v - c;
		var (r, g, b) = h switch { < 60 => (c, x, 0d), < 120 => (x, c, 0d), < 180 => (0d, c, x), < 240 => (0d, x, c), < 300 => (x, 0d, c), _ => (c, 0d, x) };
		return Color.FromRgb((byte)((r + m) * 255), (byte)((g + m) * 255), (byte)((b + m) * 255));
	}

	private static void Place(Canvas canvas, Control control, double x, double y)
	{
		Canvas.SetLeft(control, x);
		Canvas.SetTop(control, y);
		canvas.Children.Add(control);
	}
}
