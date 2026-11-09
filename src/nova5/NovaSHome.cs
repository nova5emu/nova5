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
	// The console around the screen.
	private static readonly Color Desk  = Color.Parse("#F2F3F5");
	private static readonly Color Bezel = Color.Parse("#101115");
	private static readonly Color Red   = Color.Parse("#FF4554");
	private static readonly Color Blue  = Color.Parse("#00C3E3");
	private static readonly Color Knob  = Color.Parse("#24262C");
	private const double JoyWidth = 250, BezelSize = 30;

	private readonly Grid _screen = new() { Width = 1920, Height = 1080, ClipToBounds = true, Background = new SolidColorBrush(Back) };
	private readonly Grid _settingsHost = new() { IsVisible = false, Background = new SolidColorBrush(Colors.Black, 0.55) };

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
			new("", L.T("Añadir carpeta"), () => AddFolder?.Invoke()),
			new("", L.T("Claves"), () => InstallKeys?.Invoke()),
			new("", "Firmware", () => InstallFirmware?.Invoke()),
			// The games' saves: opens the folder to copy them or bring them from another PC.
			new("", L.T("Partidas"), Services.SwitchGames.OpenSavesFolder),
			new("", L.T("Portada"), () => { if (_index < _games.Count) ChangeCover?.Invoke(_games[_index]); else Toast(L.T("Elige primero un juego")); }),
			new("", L.T("Ajustes NovaS"), OpenSettings),
			new("", L.T("Donar"), () => System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("https://ko-fi.com/nova5emu") { UseShellExecute = true })),
			// NovaS on its own has no PS5 side to go back to: its last button closes it.
			Edition.NovaSOnly ? new("", L.T("Salir"), () => Leave?.Invoke()) : new("", "Nova5 (PS5)", () => Leave?.Invoke()),
		};

		var stage = new Canvas { Width = 1920, Height = 1080 };

		// Top: NovaS's red and blue mark and its name on the left, clock and controller on the right.
		var who = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 18 };
		who.Children.Add(MainWindow.NovaSMark(62));
		who.Children.Add(Label("NovaS", 34, FontWeight.Bold, TextColor));
		Place(stage, who, RowLeft, 54);

		var status = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 30, VerticalAlignment = VerticalAlignment.Center };
		status.Children.Add(_pad);
		status.Children.Add(_clock);
		var statusDock = new DockPanel { Width = 1920 - RowLeft * 2, LastChildFill = false };
		DockPanel.SetDock(status, Dock.Right);
		statusDock.Children.Add(status);
		Place(stage, statusDock, RowLeft, 66);
		// A new version: its own line under the clock, clear of the window bar in the middle.
		_updateNotice.Child = _updateNoticeText;
		_updateNotice.PointerPressed += (_, _) => _updateRun?.Invoke();
		var noticeDock = new DockPanel { Width = 1920 - RowLeft * 2, LastChildFill = false };
		DockPanel.SetDock(_updateNotice, Dock.Right);
		noticeDock.Children.Add(_updateNotice);
		Place(stage, noticeDock, RowLeft, 128);

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
		hints.Children.Add(Hint("A", L.T("Iniciar")));
		hints.Children.Add(Hint("Y", L.T("Ajustes")));
		if (!Edition.NovaSOnly) hints.Children.Add(Hint("B", "Nova5"));
		var hintsDock = new DockPanel { Width = 1920 - RowLeft * 2, LastChildFill = false };
		var console = Label(L.T("Juegos de Switch 2"), 20, FontWeight.Normal, Dim);
		DockPanel.SetDock(console, Dock.Left);
		DockPanel.SetDock(hints, Dock.Right);
		hintsDock.Children.Add(console);
		hintsDock.Children.Add(hints);
		Place(stage, hintsDock, RowLeft, 1000);

		_toast.Child = _toastText;
		var toastDock = new Panel { Width = 1920 };
		toastDock.Children.Add(_toast);
		Place(stage, toastDock, 0, 680);

		// The home is the screen of a console: a red controller on the left, a blue one on the right, on a light desk
		// (the same look as NovaS's web page). The drawn buttons work like the real ones.
		_screen.Children.Add(stage);
		_screen.Children.Add(_settingsHost);
		_screen.Children.Add(_notesHost);
		_joyLeft  = JoyCon(left: true);
		_joyRight = JoyCon(left: false);
		_bezel.Child = _screen;
		var device = new StackPanel { Orientation = Orientation.Horizontal };
		device.Children.Add(_joyLeft);
		device.Children.Add(_bezel);
		device.Children.Add(_joyRight);
		_shadow.Child = device;
		Children.Add(new Viewbox { Stretch = Stretch.Uniform, Child = _shadow });
		// When NovaS is the window (no frame): this small bar on top of the screen minimizes, enlarges and closes it.
		_screen.Children.Add(_windowBar = WindowBar());
		Look = Edition.NovaSOnly ? NovaSLook.Console : NovaSLook.Desk;

		var clock =new DispatcherTimer(TimeSpan.FromSeconds(1), DispatcherPriority.Background, (_, _) => _clock.Text = DateTime.Now.ToString("HH:mm"));
		clock.Start();
		_clock.Text = DateTime.Now.ToString("HH:mm");
	}

	public string PadStatus { set => _pad.Text = value; }

	// The console's size with the small margin its shadow needs: NovaS on its own sizes its window to it.
	public const double WindowMargin = 26;
	public static double ConsoleAspect => (JoyWidth * 2 + 1920 + WindowMargin * 2) / (1080 + BezelSize * 2 + WindowMargin * 2);

	private readonly Border _shadow = new() { CornerRadius = new CornerRadius(JoyWidth * 0.6) };
	private readonly Border _bezel  = new() { Background = new SolidColorBrush(Bezel) };
	private Border _joyLeft = new(), _joyRight = new();
	private TextBlock? _sizeGlyph;
	private Border? _windowBar;
	private NovaSLook _look;

	// Desk: inside Nova5, the console on a wide light desk. Console: NovaS's own see-through window, only the console is
	// drawn. Screen: NovaS full screen, no controllers, just the screen as before.
	public enum NovaSLook { Desk, Console, Screen }

	public NovaSLook Look
	{
		set
		{
			_look = value;
			var screen = value == NovaSLook.Screen;
			if (_windowBar is not null) _windowBar.IsVisible = value != NovaSLook.Desk;
			Background = value switch
			{
				NovaSLook.Desk    => new SolidColorBrush(Desk),
				NovaSLook.Console => Brushes.Transparent,
				_                 => new SolidColorBrush(Back),
			};
			_joyLeft.IsVisible = _joyRight.IsVisible = !screen;
			_bezel.Padding = screen ? new Thickness(0) : new Thickness(0, BezelSize);
			_shadow.CornerRadius = screen ? new CornerRadius(0) : new CornerRadius(JoyWidth * 0.6);
			_shadow.Margin = value switch
			{
				NovaSLook.Desk    => new Thickness(90, 70),
				NovaSLook.Console => new Thickness(WindowMargin, WindowMargin * 0.4, WindowMargin, WindowMargin * 1.6),
				_                 => new Thickness(0),
			};
			_shadow.BoxShadow = value switch
			{
				NovaSLook.Desk    => BoxShadows.Parse("0 34 70 0 #4426282E"),
				NovaSLook.Console => BoxShadows.Parse("0 10 22 0 #5526282E"),
				_                 => default,
			};
			if (_sizeGlyph is not null) _sizeGlyph.Text = screen ? "\uE73F" : "\uE740"; // Back to the console / full screen.
		}
	}

	// The window bar's requests (NovaS on its own) and dragging the window by the bar or the controllers' bodies.
	public event System.Action? MinimizeRequested, FullScreenRequested, CloseRequested;
	public event System.Action<PointerPressedEventArgs>? MoveRequested;

	private Border WindowBar()
	{
		var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 14 };
		Border Round(string glyph, Color hover, System.Action run, out TextBlock label)
		{
			label = Label(glyph, 26, FontWeight.Normal, TextColor, Icons, center: true);
			var round = new Border
			{
				Width = 62, Height = 62, CornerRadius = new CornerRadius(31), Background = new SolidColorBrush(Panel),
				Child = label, Cursor = new Cursor(StandardCursorType.Hand),
			};
			round.PointerEntered += (_, _) => round.Background = new SolidColorBrush(hover);
			round.PointerExited  += (_, _) => round.Background = new SolidColorBrush(Panel);
			round.PointerPressed += (_, e) => { e.Handled = true; run(); };
			return round;
		}
		buttons.Children.Add(Round("\uE921", Line, () => MinimizeRequested?.Invoke(), out _));
		buttons.Children.Add(Round("\uE740", Line, () => FullScreenRequested?.Invoke(), out var size));
		buttons.Children.Add(Round("\uE8BB", Color.Parse("#E5484D"), () => CloseRequested?.Invoke(), out _));
		_sizeGlyph = size;
		var bar = new Border
		{
			Child = buttons, Padding = new Thickness(20, 10), CornerRadius = new CornerRadius(41), Margin = new Thickness(0, 12, 0, 0),
			Background = new SolidColorBrush(Color.Parse("#1D1E24"), 0.92), BorderBrush = new SolidColorBrush(Line), BorderThickness = new Thickness(1),
			HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Top, Cursor = new Cursor(StandardCursorType.SizeAll),
		};
		bar.PointerPressed += (_, e) => { if (!e.Handled) MoveRequested?.Invoke(e); };
		return bar;
	}

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

	// A newer version was found (NovaS on its own): a red notice at the top, by the clock, and a red round button first
	// in the row; both install it.
	private readonly TextBlock _updateNoticeText = Label("", 22, FontWeight.Bold, Colors.White);
	private readonly Border _updateNotice = new()
	{
		IsVisible = false, Padding = new Thickness(22, 9), CornerRadius = new CornerRadius(24), Background = new SolidColorBrush(Red),
		Cursor = new Cursor(StandardCursorType.Hand), VerticalAlignment = VerticalAlignment.Center,
	};
	private System.Action? _updateRun;

	public void OfferUpdate(string label, System.Action run, string? notice = null)
	{
		_buttons.RemoveAll(b => b.Glyph == UpdateGlyph);
		_buttons.Insert(0, new(UpdateGlyph, label, run));
		_updateRun = run;
		_updateNoticeText.Text = notice ?? label;
		_updateNotice.IsVisible = true;
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
			head.Children.Add(Label(L.T("Ajustes de NovaS"), 40, FontWeight.Bold, TextColor));
			var close = Hint("B", L.T("Volver"));
			close.Cursor = new Cursor(StandardCursorType.Hand);
			close.PointerPressed += (_, _) => CloseSettings();
			DockPanel.SetDock(close, Dock.Right);
			head.Children.Add(close);
			body.Children.Add(head);
			body.Children.Add(Label(L.T("▲ ▼ / rueda del ratón elegir y desplazar     ◂ ▸ / A / clic cambiar     B volver"), 20, FontWeight.Normal, Dim));
			_settingsScroll.Content = _settingsRows;
			body.Children.Add(_settingsScroll);
			_settingsSheet = new Border
			{
				Width = 1240, Padding = new Thickness(48, 40), CornerRadius = new CornerRadius(28), Child = body,
				Background = new SolidColorBrush(Panel), BorderBrush = new SolidColorBrush(Line), BorderThickness = new Thickness(1),
				HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center,
			};
			_settingsHost.Children.Add(_settingsSheet); // Over the home, inside the console's screen.
		}
		_setting = 0;
		BuildSettings();
		_settingsScroll.Offset = default;
		_settingsHost.IsVisible = true;
	}

	public bool SettingsOpen => _settingsHost.IsVisible;

	private void CloseSettings() => _settingsHost.IsVisible = false;

	// The launcher's own rows first (language, updates; the launcher keeps them), then the engine's options.
	public sealed record LauncherRow(string Label, Func<string> Detail, Func<string> Value, System.Action<int> Change);
	public List<LauncherRow> LauncherRows { get; } = new();
	private int SettingCount => LauncherRows.Count + SwitchGames.Options.Length;

	public void RefreshSettings()
	{
		if (SettingsOpen) BuildSettings();
	}

	private void BuildSettings()
	{
		_settingsRows.Children.Clear();
		for (var i = 0; i < SettingCount; i++)
		{
			var index  = i;
			var launcher = i < LauncherRows.Count ? LauncherRows[i] : null;
			var option = launcher is null ? SwitchGames.Options[i - LauncherRows.Count] : null;
			var focused = i == _setting;
			var row = new DockPanel { LastChildFill = true };
			var shown = launcher is not null ? launcher.Value() : option!.Choices[SwitchGames.Current(option)].Name;
			var value = Label("‹  " + shown + "  ›", 24, FontWeight.SemiBold, focused ? Accent : TextColor);
			DockPanel.SetDock(value, Dock.Right);
			row.Children.Add(value);
			var text = new StackPanel { Spacing = 4, HorizontalAlignment = HorizontalAlignment.Left };
			text.Children.Add(Label(launcher?.Label ?? option!.Label, 26, FontWeight.SemiBold, TextColor));
			text.Children.Add(new TextBlock { Text = launcher?.Detail() ?? option!.Detail, FontSize = 18, TextWrapping = TextWrapping.Wrap, Foreground = new SolidColorBrush(Dim), MaxWidth = 760, HorizontalAlignment = HorizontalAlignment.Left });
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

	// Snapshots: the settings open on the row whose label starts with this text.
	public void FocusSetting(string label)
	{
		var index = LauncherRows.FindIndex(r => r.Label.StartsWith(label, StringComparison.OrdinalIgnoreCase));
		var option = Array.FindIndex(SwitchGames.Options, o => o.Label.StartsWith(label, StringComparison.OrdinalIgnoreCase));
		if (index < 0 && option >= 0) index = LauncherRows.Count + option;
		if (index >= 0) SelectSetting(index);
	}

	// Pad and keyboard move the selection; keep it on screen.
	private void SelectSetting(int index)
	{
		_setting = Math.Clamp(index, 0, SettingCount - 1);
		BuildSettings();
		var row = (Control)_settingsRows.Children[_setting];
		Dispatcher.UIThread.Post(() => row.BringIntoView(), DispatcherPriority.Background);
	}

	private void Change(int step)
	{
		if (_setting < LauncherRows.Count)
		{
			LauncherRows[_setting].Change(step);
			BuildSettings();
			return;
		}
		var option = SwitchGames.Options[_setting - LauncherRows.Count];
		var next = (SwitchGames.Current(option) + step + option.Choices.Length) % option.Choices.Length;
		try
		{
			SwitchGames.Set(option, next);
		}
		catch (Exception e) when (e is IOException or UnauthorizedAccessException)
		{
			Toast(L.T("No se pudo guardar: ") + e.Message);
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

	// ---- Release notes (once after an update, and from Ajustes › Novedades) ------------------------

	private readonly Grid _notesHost = new() { IsVisible = false, Background = new SolidColorBrush(Colors.Black, 0.55) };
	private readonly StackPanel _notesList = new() { Spacing = 34 };
	private readonly ScrollViewer _notesScroll = new()
	{
		MaxHeight = 600, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
		VerticalScrollBarVisibility = ScrollBarVisibility.Auto, Padding = new Thickness(0, 0, 18, 0),
	};
	private readonly TextBlock _notesTitle = Label("", 40, FontWeight.Bold, TextColor);
	private Border? _notesSheet;

	public bool NotesOpen => _notesHost.IsVisible;
	public event System.Action? NotesClosed;

	public void ShowNotes(string title, IReadOnlyList<Services.ReleaseNote> notes)
	{
		if (notes.Count == 0) return;
		if (_notesSheet is null)
		{
			var body = new StackPanel { Spacing = 22 };
			var head = new DockPanel { LastChildFill = true };
			var close = Hint("B", L.T("Cerrar"));
			close.Cursor = new Cursor(StandardCursorType.Hand);
			close.PointerPressed += (_, _) => CloseNotes();
			DockPanel.SetDock(close, Dock.Right);
			head.Children.Add(close);
			_notesTitle.TextWrapping = TextWrapping.Wrap;
			head.Children.Add(_notesTitle);
			body.Children.Add(head);
			body.Children.Add(Label(L.T("▲ ▼ / rueda del ratón desplazar     A o B cerrar"), 20, FontWeight.Normal, Dim));
			_notesScroll.Content = _notesList;
			body.Children.Add(_notesScroll);
			_notesSheet = new Border
			{
				Width = 1240, Padding = new Thickness(48, 40), CornerRadius = new CornerRadius(28), Child = body,
				Background = new SolidColorBrush(Panel), BorderBrush = new SolidColorBrush(Line), BorderThickness = new Thickness(1),
				HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center,
				Margin = new Thickness(0, 110, 0, 0), // Below the window bar.
			};
			_notesSheet.PointerPressed += (_, e) => e.Handled = true; // Clicks inside do not reach the backdrop.
			_notesHost.PointerPressed += (_, _) => CloseNotes();
			_notesHost.Children.Add(_notesSheet);
		}
		_notesTitle.Text = title;
		_notesList.Children.Clear();
		foreach (var note in notes)
		{
			var block = new StackPanel { Spacing = 10 };
			var date  = DateTime.TryParse(note.Date, out var day) ? " · " + day.ToString(L.T("d 'de' MMMM 'de' yyyy"), new System.Globalization.CultureInfo(L.T("es-ES"))) : "";
			block.Children.Add(Label(L.F("VERSIÓN {0}{1}", note.Version, date), 18, FontWeight.SemiBold, Accent));
			block.Children.Add(new TextBlock { Text = note.Title, FontSize = 30, FontWeight = FontWeight.Bold, Foreground = new SolidColorBrush(TextColor), TextWrapping = TextWrapping.Wrap });
			foreach (var item in note.Items)
			{
				var line = new DockPanel { Margin = new Thickness(0, 4, 0, 0) };
				var dot  = new Ellipse { Width = 10, Height = 10, Fill = new SolidColorBrush(Red), VerticalAlignment = VerticalAlignment.Top, Margin = new Thickness(4, 12, 18, 0) };
				DockPanel.SetDock(dot, Dock.Left);
				line.Children.Add(dot);
				line.Children.Add(new TextBlock { Text = item, FontSize = 23, LineHeight = 33, Foreground = new SolidColorBrush(TextColor, 0.88), TextWrapping = TextWrapping.Wrap });
				block.Children.Add(line);
			}
			_notesList.Children.Add(block);
		}
		_notesScroll.Offset = default;
		_notesHost.IsVisible = true;
	}

	private void CloseNotes()
	{
		if (!NotesOpen) return;
		_notesHost.IsVisible = false;
		NotesClosed?.Invoke();
	}

	private void NotesInput(PadInput input)
	{
		switch (input)
		{
			case PadInput.Up:   _notesScroll.Offset = _notesScroll.Offset.WithY(Math.Max(0, _notesScroll.Offset.Y - 140)); break;
			case PadInput.Down: _notesScroll.Offset = _notesScroll.Offset.WithY(_notesScroll.Offset.Y + 140); break;
			case PadInput.Accept or PadInput.Back or PadInput.Options or PadInput.Menu: CloseNotes(); break;
		}
	}

	// Pad and keyboard.
	public void Input(PadInput input)
	{
		if (NotesOpen)
		{
			NotesInput(input);
			return;
		}
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
			    ? L.F("{0} · no necesita claves · {1:0.0} GB", (game.Path.EndsWith(".nup", StringComparison.OrdinalIgnoreCase) ? L.T("Paquete NUP") : L.T("Carpeta descifrada")), game.Size / 1e9)
			    : $"{System.IO.Path.GetExtension(game.Path).TrimStart('.').ToUpperInvariant()} · {game.Size / 1e9:0.0} GB";
		}
		else
		{
			_name.Text   = _games.Count == 0 ? L.T("Añade tus juegos de Switch 2") : L.T("Añadir carpeta");
			_detail.Text = L.T("Elige la carpeta con tus .nsp, .xci, .nup o juegos descifrados (exefs + romfs)");
		}

		// Extracted (decrypted) games boot without keys; .nsp/.xci need the console's own.
		var needsKeys = _games.Count == 0 || _games.Any(g => !g.Extracted);
		var missingKeys = needsKeys && !SwitchGames.HasKeys;
		_banner.IsVisible = !SwitchGames.HasEngine || missingKeys || !SwitchGames.HasFirmware;
		_bannerText.Text = !SwitchGames.HasEngine
		    ? L.F("Falta el motor de NovaS. Actualiza {0} para instalarlo.", Edition.Name)
		    : missingKeys
		    ? L.T("Los .nsp y .xci necesitan las claves de TU consola: pulsa \"Claves\" y elige tu prod.keys (y title.keys si lo tienes). ") +
		      L.T("Los .nup y los juegos en carpeta descifrada (exefs + romfs) no las necesitan.")
		    : L.T("Recomendado: instala el firmware de tu consola con \"Firmware\" (elige su .zip o cualquier .nca de su carpeta). ") +
		      L.T("Muchos juegos arrancan sin él, pero algunos lo necesitan para fuentes, teclado y menús.");
	}

	// ---- Pieces -------------------------------------------------------------------------------

	// One controller beside the screen: red on the left (minus, stick, d-pad, capture), blue on the right (plus, A B X Y,
	// stick, home). Clicking its buttons moves and chooses like a pad.
	private Border JoyCon(bool left)
	{
		const double height = 1080 + BezelSize * 2, round = JoyWidth * 0.6;
		var parts = new StackPanel { Spacing = 64, HorizontalAlignment = HorizontalAlignment.Center, Margin = new Thickness(0, 120, 0, 0) };

		// Minus or plus.
		var sign = new Grid { Width = 56, Height = 56, HorizontalAlignment = HorizontalAlignment.Center };
		sign.Children.Add(new Rectangle { Width = 56, Height = 12, RadiusX = 6, RadiusY = 6, Fill = new SolidColorBrush(Knob) });
		if (!left) sign.Children.Add(new Rectangle { Width = 12, Height = 56, RadiusX = 6, RadiusY = 6, Fill = new SolidColorBrush(Knob) });
		var stick = PadButton(new Ellipse
		{
			Width = 132, Height = 132, StrokeThickness = 8, Stroke = new SolidColorBrush(Colors.Black, 0.25),
			Fill = new RadialGradientBrush
			{
				GradientOrigin = new RelativePoint(0.4, 0.35, RelativeUnit.Relative),
				GradientStops = { new GradientStop(Color.Parse("#4A4D56"), 0), new GradientStop(Color.Parse("#1F2025"), 0.75) },
			},
		}, left ? PadInput.Left : PadInput.Right);

		// Four round buttons in a diamond: the d-pad on the left, A B X Y on the right.
		var diamond = new Canvas { Width = 176, Height = 176, HorizontalAlignment = HorizontalAlignment.Center };
		(string Label, PadInput? Input, double X, double Y)[] keys = left
		    ? new (string, PadInput?, double, double)[] { ("\uE70E", PadInput.Up, 58, 0), ("\uE76B", PadInput.Left, 0, 58), ("\uE76C", PadInput.Right, 116, 58), ("\uE70D", PadInput.Down, 58, 116) }
		    : new (string, PadInput?, double, double)[] { ("X", null, 58, 0), ("Y", PadInput.Options, 0, 58), ("A", PadInput.Accept, 116, 58), ("B", PadInput.Back, 58, 116) };
		foreach (var key in keys)
		{
			var face = new Border
			{
				Width = 60, Height = 60, CornerRadius = new CornerRadius(30), Background = new SolidColorBrush(Knob),
				Child = Label(key.Label, left ? 20 : 24, FontWeight.Bold, Color.Parse("#E9EAEE"), left ? Icons : null, center: true),
			};
			var button = key.Input is { } input ? PadButton(face, input) : face;
			Canvas.SetLeft(button, key.X);
			Canvas.SetTop(button, key.Y);
			diamond.Children.Add(button);
		}
		var small = new Border
		{
			Width = 50, Height = 50, HorizontalAlignment = HorizontalAlignment.Center, Background = new SolidColorBrush(Knob),
			CornerRadius = new CornerRadius(left ? 14 : 25), BorderThickness = new Thickness(left ? 0 : 5), BorderBrush = new SolidColorBrush(Color.Parse("#3A3D45")),
		};

		parts.Children.Add(sign);
		if (left) { parts.Children.Add(stick); parts.Children.Add(diamond); }
		else      { parts.Children.Add(diamond); parts.Children.Add(stick); }
		parts.Children.Add(small);
		var body = new Border
		{
			Width = JoyWidth, Height = height, Child = parts, Background = new SolidColorBrush(left ? Red : Blue),
			CornerRadius = left ? new CornerRadius(round, 0, 0, round) : new CornerRadius(0, round, round, 0),
		};
		// NovaS as the window: the controllers' bodies (not their buttons) move it; a double click enlarges it.
		body.PointerPressed += (_, e) =>
		{
			if (e.Handled || _look != NovaSLook.Console) return;
			if (e.ClickCount == 2) FullScreenRequested?.Invoke();
			else MoveRequested?.Invoke(e);
		};
		return body;
	}

	// A drawn controller button: it shrinks a little while pressed and acts like that pad input.
	private Control PadButton(Control face, PadInput input)
	{
		face.Cursor = new Cursor(StandardCursorType.Hand);
		face.HorizontalAlignment = HorizontalAlignment.Center;
		face.RenderTransformOrigin = new RelativePoint(0.5, 0.5, RelativeUnit.Relative);
		face.PointerPressed  += (_, e) => { face.RenderTransform = new ScaleTransform(0.9, 0.9); Input(input); e.Handled = true; };
		face.PointerReleased += (_, _) => face.RenderTransform = null;
		face.PointerExited   += (_, _) => face.RenderTransform = null;
		return face;
	}

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
		var update = action.Glyph == UpdateGlyph; // Stands out: red, like the notice at the top.
		panel.Children.Add(new Border
		{
			Width = 92, Height = 92, CornerRadius = new CornerRadius(46), Background = new SolidColorBrush(update ? Red : Panel),
			BorderThickness = new Thickness(4), BorderBrush = new SolidColorBrush(Line), HorizontalAlignment = HorizontalAlignment.Center,
			Child = Label(action.Glyph, 36, FontWeight.Normal, update ? Colors.White : TextColor, Icons, center: true),
		});
		panel.Children.Add(Label(action.Label, 18, update ? FontWeight.Bold : FontWeight.Normal, update ? TextColor : Dim, center: true));
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
