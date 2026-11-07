using System.Diagnostics;
using Avalonia;
using Avalonia.Animation;
using Avalonia.Controls;
using Avalonia.Controls.Shapes;
using Avalonia.Input;
using Avalonia.Input.Platform;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform.Storage;
using Avalonia.Threading;
using Nova5.Services;

namespace Nova5;

public sealed class MainWindow : Window
{
	private enum Zone { Tiles, Buttons, Settings, Prompt, Notes, GameOptions, Trophies, TrophyList, TextEntry, Keys, Choice, NovaS }

	private const int TrophiesTab = 1;
	private const int SettingsTab = 2;
	private const int NovaSTab    = 3; // The Switch 2 side, with its own home screen (NovaSHome).

	private const double TileSize   = 168;
	private const double FocusSize  = 236;
	private const double TileGap    = 28;
	private const int    VisibleTiles = 6;

	private readonly Emulator  _emulator = new();
	private readonly Store     _store    = Store.Load();
	private readonly MenuMusic _music    = new();
	private readonly Gamepad   _pad      = new();
	private readonly Dictionary<string, Bitmap?> _images = new();

	private List<Game> _games = new();
	private int        _index;
	private int        _button;
	private int        _setting;
	private int        _tab;
	private Zone       _zone = Zone.Tiles;
	private Zone       _beforePrompt;
	private Process?   _running;
	private Game?      _runningGame;
	private DateTime   _started;

	private readonly Image      _backA = NewBackground(), _backB = NewBackground();
	private bool                _showingA = true;
	private string?             _backPath;
	private readonly StackPanel _row = new() { Orientation = Orientation.Horizontal, Spacing = TileGap };
	private readonly TranslateTransform _rowShift = new();
	private readonly TextBlock  _title = Text("", 76, FontWeight.Bold);
	private readonly StackPanel _meta  = new() { Orientation = Orientation.Horizontal, Spacing = 14 };
	private readonly Border[]   _buttons;
	private readonly StackPanel _stats = new() { Spacing = 0 };
	private readonly Panel      _gamesPage = new();
	private readonly StackPanel _settingsList = new() { Spacing = 6 };
	private readonly StackPanel _settingsCategories = new() { Spacing = 4 };
	private bool                _settingsOnCategories = true; // The pad is on the category column (left) or on its options (right).
	private readonly ScrollViewer _settingsScroll = new() { HorizontalScrollBarVisibility = Avalonia.Controls.Primitives.ScrollBarVisibility.Disabled };
	private readonly Panel      _settingsPage = new() { IsVisible = false };
	private readonly StackPanel _notesList = new() { Spacing = 34 };
	private readonly ScrollViewer _notesScroll = new() { HorizontalScrollBarVisibility = Avalonia.Controls.Primitives.ScrollBarVisibility.Disabled };
	private readonly TextBlock  _notesTitle = Text("", 44, FontWeight.Black);
	private Border              _notes = new();
	private Zone                _beforeNotes;
	private readonly StackPanel _gameList = new() { Spacing = 6 };
	private readonly ScrollViewer _gameScroll = new() { HorizontalScrollBarVisibility = Avalonia.Controls.Primitives.ScrollBarVisibility.Disabled };
	private readonly TextBlock  _gameTitle = Text("", 44, FontWeight.Black);
	private Border              _gameSheet = new();
	private Game?               _optionsGame;
	private GamePatches?        _patches;
	private int                 _gameOption;
	private bool                _downloadingPatches;
	private Game?               _packing;
	private double              _packProgress;
	private string?             _confirmDelete;
	private CancellationTokenSource? _packCancel;
	private Game?                    _prepping;       // The game whose shaders are being prepared (one at a time).
	private double                   _prepProgress;
	private bool                     _prepReady;
	private CancellationTokenSource? _prepCancel;
	private readonly TextBlock  _prepText = Text("", 20, FontWeight.SemiBold, Palette.AuroraA);
	private Border              _prepPill = new();
	private readonly TextBlock  _packText = Text("", 20, FontWeight.SemiBold, Palette.AuroraA);
	private Border              _packPill = new();
	private (string Folder, string Archive)? _zarTest;
	private Border              _choiceSheet = new();
	private readonly TextBlock  _choiceTitle = Text("", 40, FontWeight.Black);
	private readonly TextBlock  _choiceMessage = new() { FontSize = 22, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 0, 0, 14) };
	private readonly StackPanel _choiceList = new() { Spacing = 6 };
	private List<Setting>       _choices = new();
	private int                 _choiceIndex;
	private Zone                _beforeChoice;
	private readonly Border     _gameTrophies = new() { IsVisible = false };
	private readonly Dictionary<string, TrophySet?> _trophyCache = new(StringComparer.OrdinalIgnoreCase);
	private TrophySet?          _stripSet;
	private readonly Panel      _trophiesPage = new() { IsVisible = false };
	private Border?             _trophyPanel; // Hidden under a game's trophy list, which would show through it.
	private readonly StackPanel _trophyGames = new() { Spacing = 6 };
	private readonly ScrollViewer _trophyScroll = new() { HorizontalScrollBarVisibility = Avalonia.Controls.Primitives.ScrollBarVisibility.Disabled };
	private readonly StackPanel _trophySummary = new() { Orientation = Orientation.Horizontal, Spacing = 26, Margin = new Thickness(0, 0, 0, 18) };
	private List<TrophySet>     _trophySets = new();
	private int                 _trophyIndex;
	private readonly StackPanel _trophyList = new() { Spacing = 6 };
	private readonly ScrollViewer _trophyListScroll = new() { HorizontalScrollBarVisibility = Avalonia.Controls.Primitives.ScrollBarVisibility.Disabled };
	private readonly TextBlock  _trophyListTitle = Text("", 44, FontWeight.Black);
	private readonly StackPanel _trophyListSummary = new() { Orientation = Orientation.Horizontal, Spacing = 26, Margin = new Thickness(0, 0, 0, 18) };
	private Border              _trophySheet = new();
	private Border              _textSheet = new();
	private readonly TextBlock  _textTitle = Text("", 40, FontWeight.Black);
	private readonly TextBox    _textBox = new() { FontSize = 30, MaxLength = 16, Width = 900, Padding = new Thickness(18, 12) };
	private Action<string>?     _textDone;
	private Zone                _beforeText;
	private Border              _keysSheet = new();
	private readonly StackPanel _keysList = new() { Spacing = 6 };
	private readonly ScrollViewer _keysScroll = new() { HorizontalScrollBarVisibility = Avalonia.Controls.Primitives.ScrollBarVisibility.Disabled };
	private Dictionary<string, string> _keymap = new();
	private int                 _keyIndex;
	private string?             _capturing;
	private readonly TextBlock[] _tabs;
	private readonly TextBlock  _clock = Text("", 22, FontWeight.SemiBold);
	private readonly TextBlock  _padStatus = Text("Sin mando", 20, FontWeight.Normal, Palette.Muted);
	private readonly TextBlock  _toast = Text("", 24, FontWeight.Normal);
	private readonly Border     _toastBox;
	private readonly Border     _prompt;
	private Border?             _statsBox;
	private Border              _chrome = new();
	private bool                _chromePinned;
	private readonly TextBlock  _fullscreenLabel = Text("", 20, FontWeight.Normal);
	private readonly TextBlock  _updateText = Text("", 20, FontWeight.SemiBold, Palette.AuroraA);
	private Border              _updatePill = new();
	private readonly NovaSHome  _novas = new();
	private UpdateInfo?         _update;
	private bool                _updating;

	public MainWindow()
	{
		Title      = Edition.Name;
		Background = Palette.Brush(Palette.Night);
		FontFamily = Palette.Font;
		Width      = 1600;
		Height     = 900;
		WindowState = SnapshotPath is null ? WindowState.FullScreen : WindowState.Normal;

		_buttons = new[] { ActionButton("Jugar", primary: true), ActionButton("Opciones del juego"), ActionButton("Abrir carpeta") };
		_tabs    = new[] { Text("Juegos", 24, FontWeight.SemiBold), Text("Trofeos", 24, FontWeight.Normal, Palette.Muted), Text("Ajustes", 24, FontWeight.Normal, Palette.Muted), Text("NovaS · Switch 2", 24, FontWeight.Normal, Palette.Muted) };
		_novas.Launch    += LaunchSwitch;
		_novas.InstallKeys     += InstallSwitchKeys;
		_novas.InstallFirmware += InstallSwitchFirmware;
		_novas.ChangeCover     += ChangeSwitchCover;
		_novas.Leave     += () => { if (Edition.NovaSOnly) Close(); else ShowTab(0); };
		_novas.AddFolder += AddSwitchFolder;
		_toastBox = new Border
		{
			Child = _toast, Padding = new Thickness(28, 14), CornerRadius = new CornerRadius(16), Opacity = 0,
			Background = Palette.Brush(Palette.Ink, 0.92), BorderBrush = Palette.Brush(Colors.White, 0.15), BorderThickness = new Thickness(1),
			Transitions = new Transitions { new DoubleTransition { Property = OpacityProperty, Duration = TimeSpan.FromMilliseconds(250) } },
		};
		_prompt = BuildPrompt();
		_notes  = BuildNotes();
		_gameSheet = BuildGameOptions();
		_trophySheet = BuildTrophySheet();
		_textSheet = BuildTextEntry();
		_keysSheet = BuildKeys();
		_choiceSheet = BuildChoice();
		Content = BuildLayout();

		_music.Enabled = _store.Music;
		_music.Volume  = _store.MusicVolume;
		if (!Gamepad.Initialize()) _padStatus.Text = "Mando no disponible";
		_pad.Input     += OnInput;
		_pad.Connected += name => _novas.PadStatus = _padStatus.Text = name is null ? "Sin mando" : $"Mando: {name}";
		_novas.PadStatus = _padStatus.Text;

		var poll = new DispatcherTimer(TimeSpan.FromMilliseconds(16), DispatcherPriority.Input, (_, _) => _pad.Poll(IsActive && _running is null));
		poll.Start();
		var clock = new DispatcherTimer(TimeSpan.FromSeconds(1), DispatcherPriority.Background, (_, _) => _clock.Text = DateTime.Now.ToString("HH:mm"));
		clock.Start();
		_clock.Text = DateTime.Now.ToString("HH:mm");

		KeyDown += OnKey;
		PointerMoved += (_, e) =>
		{
			var y = e.GetPosition(this).Y;
			if (y <= 6) ShowChrome(true);
			else if (!_chromePinned && y > 72) ShowChrome(false);
		};
		PointerWheelChanged += (_, e) =>
		{
			if (_running is not null || _zone is Zone.Prompt or Zone.Notes or Zone.GameOptions or Zone.TrophyList or Zone.TextEntry or Zone.Keys or Zone.Choice) return;
			if (_tab == NovaSTab && _novas.SettingsOpen) return; // Its list scrolls with the wheel.
			var sideways = _tab is 0 or NovaSTab;
			OnInput(e.Delta.Y < 0 ? (sideways ? PadInput.Right : PadInput.Down) : (sideways ? PadInput.Left : PadInput.Up));
		};
		PropertyChanged += (_, e) =>
		{
			if (e.Property == WindowStateProperty) _fullscreenLabel.Text = WindowState == WindowState.FullScreen ? "Ventana" : "Pantalla completa";
		};
		Opened  += (_, _) =>
		{
			Reload();
			_fullscreenLabel.Text = WindowState == WindowState.FullScreen ? "Ventana" : "Pantalla completa";
			if (SnapshotPath is { } snapshot)
			{
				if (SnapshotNotes) ShowNotes(ReleaseNotes.Since(null, Updates.Current), firstTime: false);
				if (SnapshotSettings) ShowTab(SettingsTab);
				if (SnapshotTrophies) ShowTab(TrophiesTab);
				if (SnapshotNovaS) ShowTab(NovaSTab);
				if (SnapshotNovaSSettings) { ShowTab(NovaSTab); _novas.OpenSettings(); }
				if (SnapshotTrophyList && _trophySets.FirstOrDefault() is { } first) ShowTrophyList(first);
				if (SnapshotGame && _games.FirstOrDefault(g => g.Package is null) is { } shown) ShowGameOptions(shown);
				DispatcherTimer.RunOnce(() => SaveSnapshot(snapshot), TimeSpan.FromSeconds(4));
				return;
			}
			// NovaS on its own: straight to its home, without Nova5's notes or menu music.
			if (Edition.NovaSOnly) ShowTab(NovaSTab);
			// A new version (or a first run) shows what it brings, once.
			else if (_store.SeenNotes != Updates.Current.ToString()) ShowNotes(ReleaseNotes.Since(_store.SeenNotes, Updates.Current), firstTime: true);
			if (_store.UpdateMode != "no") DispatcherTimer.RunOnce(() => CheckUpdates(false), TimeSpan.FromSeconds(3));
		};
		Closed  += (_, _) =>
		{
			_music.Dispose();
			_pad.Dispose();
		};
	}

	// ---- Layout -------------------------------------------------------------------------------

	private Control BuildLayout()
	{
		var stage = new Canvas { Width = 1920, Height = 1080 };

		var brand = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 16, VerticalAlignment = VerticalAlignment.Center };
		brand.Children.Add(Logo(52));
		brand.Children.Add(Text("Nova5", 34, FontWeight.Bold));
		brand.Children.Add(new TextBlock { Text = "EMULADOR", FontSize = 18, FontWeight = FontWeight.Light, Foreground = Palette.Brush(Palette.Muted), LetterSpacing = 3, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(6, 4, 0, 0) });
		// Opens the Ko-fi page straight away (also listed in Settings for controller users).
		var donate = Pill(Text("☕ Donar", 18, FontWeight.SemiBold, Palette.AuroraA));
		donate.BorderBrush       = Palette.Aurora();
		donate.Cursor            = new Cursor(StandardCursorType.Hand);
		donate.VerticalAlignment = VerticalAlignment.Center;
		donate.Margin            = new Thickness(12, 4, 0, 0);
		donate.PointerPressed += (_, _) => Process.Start(new ProcessStartInfo("https://ko-fi.com/nova5emu") { UseShellExecute = true });
		brand.Children.Add(donate);
		Place(stage, brand, 72, 34);

		var status = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 28, VerticalAlignment = VerticalAlignment.Center };
		_updatePill = Pill(_updateText);
		_updatePill.IsVisible   = false;
		_updatePill.BorderBrush = Palette.Aurora();
		_updatePill.Cursor      = new Cursor(StandardCursorType.Hand);
		_updatePill.PointerPressed += (_, _) => StartUpdate();
		status.Children.Add(_updatePill);
		// While a game is being packed into .zar: its progress everywhere; a click offers to cancel.
		_packPill = Pill(_packText);
		_packPill.IsVisible   = false;
		_packPill.BorderBrush = Palette.Aurora();
		_packPill.Cursor      = new Cursor(StandardCursorType.Hand);
		_packPill.PointerPressed += (_, _) => AskCancelPack();
		status.Children.Add(_packPill);
		// While a game's shaders are prepared: its progress; a click offers to cancel.
		_prepPill = Pill(_prepText);
		_prepPill.IsVisible   = false;
		_prepPill.BorderBrush = Palette.Aurora();
		_prepPill.Cursor      = new Cursor(StandardCursorType.Hand);
		_prepPill.PointerPressed += (_, _) => AskCancelPrep();
		status.Children.Add(_prepPill);
		status.Children.Add(Pill(_padStatus));
		status.Children.Add(_clock);
		var statusDock = new DockPanel { Width = 1920 - 144, LastChildFill = false };
		DockPanel.SetDock(status, Dock.Right);
		statusDock.Children.Add(status);
		Place(stage, statusDock, 72, 40);

		var tabs = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 40 };
		for (var i = 0; i < _tabs.Length; i++)
		{
			var tab = i;
			_tabs[i].Background = Brushes.Transparent;
			_tabs[i].Cursor     = new Cursor(StandardCursorType.Hand);
			_tabs[i].PointerPressed += (_, _) => { if (_zone != Zone.Prompt) ShowTab(tab); };
			tabs.Children.Add(_tabs[i]);
		}
		Place(stage, tabs, 72, 120);

		// Games page.
		var rowHost = new Canvas { Width = 1920 - 72, Height = 260, ClipToBounds = false };
		_row.RenderTransform = _rowShift;
		_rowShift.Transitions = new Transitions { new DoubleTransition { Property = TranslateTransform.XProperty, Duration = TimeSpan.FromMilliseconds(220), Easing = new Avalonia.Animation.Easings.CubicEaseOut() } };
		rowHost.Children.Add(_row);
		Place(_gamesPage, rowHost, 72, 200);

		var info = new StackPanel { Spacing = 0, Width = 1100 };
		_title.TextTrimming = TextTrimming.CharacterEllipsis;
		_title.Margin       = new Thickness(0, 0, 0, 18);
		info.Children.Add(_title);
		_meta.Margin = new Thickness(0, 0, 0, 34);
		info.Children.Add(_meta);
		var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 18 };
		for (var i = 0; i < _buttons.Length; i++)
		{
			var action = i;
			_buttons[i].Cursor = new Cursor(StandardCursorType.Hand);
			_buttons[i].PointerEntered += (_, _) => { if (_zone is Zone.Tiles or Zone.Buttons) { _zone = Zone.Buttons; FocusButton(action); SetZoneVisuals(); } };
			_buttons[i].PointerPressed += (_, _) => { if (_zone != Zone.Prompt) Activate(action); };
			actions.Children.Add(_buttons[i]);
		}
		info.Children.Add(actions);
		Place(_gamesPage, info, 72, 640);
		// The selected game's trophies, between the covers and its name.
		Place(_gamesPage, _gameTrophies, 72, 486);
		_gameTrophies.Cursor = new Cursor(StandardCursorType.Hand);
		_gameTrophies.PointerPressed += (_, _) => OpenStripTrophies();

		var statsBox = _statsBox = new Border
		{
			Width = 440, Padding = new Thickness(30, 24), CornerRadius = new CornerRadius(24),
			Background = Palette.Brush(Palette.Ink, 0.88), BorderBrush = Palette.Brush(Colors.White, 0.12), BorderThickness = new Thickness(1),
			Child = _stats,
		};
		Place(_gamesPage, statsBox, 1920 - 72 - 440, 650);
		stage.Children.Add(_gamesPage);

		// Settings page: categories on the left (as in SharpEmu), the chosen one's options on the right, scrolling like Nexo's.
		_settingsScroll.Content = _settingsList;
		var settingsBody = new Grid { ColumnDefinitions = new ColumnDefinitions("300,24,*") };
		var categoriesBox = new Border
		{
			Child = _settingsCategories, Padding = new Thickness(10), CornerRadius = new CornerRadius(22),
			Background = Palette.Brush(Colors.White, 0.04), VerticalAlignment = VerticalAlignment.Top,
		};
		Grid.SetColumn(categoriesBox, 0);
		Grid.SetColumn(_settingsScroll, 2);
		settingsBody.Children.Add(categoriesBox);
		settingsBody.Children.Add(_settingsScroll);
		var settingsPanel = Sheet("Ajustes", "▲ ▼ elegir     A entrar / cambiar     ◂ valor anterior     B volver", settingsBody, () => ShowTab(0), "B  Volver");
		settingsPanel.Width  = 1320;
		settingsPanel.Height = 790;
		Place(_settingsPage, settingsPanel, (1920 - 1320) / 2.0, 186);
		stage.Children.Add(_settingsPage);

		// Trophies page: every game's progress; A opens its list.
		var trophyContent = new DockPanel();
		DockPanel.SetDock(_trophySummary, Dock.Top);
		trophyContent.Children.Add(_trophySummary);
		_trophyScroll.Content = _trophyGames;
		trophyContent.Children.Add(_trophyScroll);
		var trophyPanel = _trophyPanel = Sheet("Trofeos", "▲ ▼ elegir     A ver trofeos     B volver", trophyContent, () => ShowTab(0), "B  Volver");
		trophyPanel.Width  = 1320;
		trophyPanel.Height = 790;
		Place(_trophiesPage, trophyPanel, (1920 - 1320) / 2.0, 186);
		stage.Children.Add(_trophiesPage);

		var hints = new DockPanel { Width = 1920 - 144, LastChildFill = false };
		var left = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 34 };
		left.Children.Add(Clickable(Hint("A", "Aceptar"), () => OnInput(PadInput.Accept)));
		left.Children.Add(Clickable(Hint("Y", "Ajustes"), () => OnInput(PadInput.Options)));
		left.Children.Add(Clickable(Hint("LB RB", "Pestañas"), () => OnInput(PadInput.NextTab)));
		var right = Clickable(Hint("B", "Atrás"), () => OnInput(PadInput.Back));
		DockPanel.SetDock(left, Dock.Left);
		DockPanel.SetDock(right, Dock.Right);
		hints.Children.Add(left);
		hints.Children.Add(right);
		Place(stage, hints, 72, 1080 - 78);

		var toastDock = new Panel { Width = 1920 };
		_toastBox.HorizontalAlignment = HorizontalAlignment.Center;
		toastDock.Children.Add(_toastBox);
		Place(stage, toastDock, 0, 1080 - 170);

		Place(stage, _prompt, 0, 0);
		Place(stage, _gameSheet, 0, 0);
		Place(stage, _trophySheet, 0, 0);
		Place(stage, _keysSheet, 0, 0);
		Place(stage, _choiceSheet, 0, 0);
		Place(stage, _textSheet, 0, 0);
		Place(stage, _notes, 0, 0);

		var root = new Grid { Background = Palette.Brush(Palette.Night) };
		root.Children.Add(_backA);
		root.Children.Add(_backB);
		root.Children.Add(new Rectangle { Fill = Palette.Fade(Palette.P(0, 0), Palette.P(1, 0), (0, 0.92), (0.38, 0.55), (0.70, 0)) });
		root.Children.Add(new Rectangle { Fill = Palette.Fade(Palette.P(0, 1), Palette.P(0, 0), (0, 0.95), (0.45, 0)) });
		root.Children.Add(new Rectangle { Fill = Palette.Fade(Palette.P(0, 0), Palette.P(0, 1), (0, 0.7), (0.18, 0)) });
		root.Children.Add(new Viewbox { Stretch = Stretch.Uniform, Child = stage });
		root.Children.Add(_novas);
		root.Children.Add(_chrome = BuildChrome());
		return root;
	}

	// Window bar that slides in when the pointer reaches the top edge or Escape is pressed.
	private Border BuildChrome()
	{
		var bar = new DockPanel { LastChildFill = false, Margin = new Thickness(20, 0) };
		var brand = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10, VerticalAlignment = VerticalAlignment.Center };
		brand.Children.Add(Logo(26));
		brand.Children.Add(Text("Nova5", 18, FontWeight.SemiBold));
		DockPanel.SetDock(brand, Dock.Left);
		bar.Children.Add(brand);
		var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, VerticalAlignment = VerticalAlignment.Center };
		buttons.Children.Add(ChromeButton(Text("Minimizar", 20, FontWeight.Normal), Palette.Brush(Colors.White, 0.12), () => WindowState = WindowState.Minimized));
		buttons.Children.Add(ChromeButton(_fullscreenLabel, Palette.Brush(Colors.White, 0.12),
		    () => WindowState = WindowState == WindowState.FullScreen ? WindowState.Maximized : WindowState.FullScreen));
		buttons.Children.Add(ChromeButton(Text("Cerrar Nova5", 20, FontWeight.SemiBold), Palette.Brush(Color.Parse("#E5484D"), 0.9), Close));
		DockPanel.SetDock(buttons, Dock.Right);
		bar.Children.Add(buttons);
		return new Border
		{
			Height = 60, VerticalAlignment = VerticalAlignment.Top, Opacity = 0, IsHitTestVisible = false, Child = bar,
			Background = Palette.Brush(Palette.Ink, 0.96), BorderBrush = Palette.Brush(Colors.White, 0.1), BorderThickness = new Thickness(0, 0, 0, 1),
			Transitions = new Transitions { new DoubleTransition { Property = OpacityProperty, Duration = TimeSpan.FromMilliseconds(160) } },
		};
	}

	private static Border ChromeButton(TextBlock label, IBrush hover, Action click)
	{
		var button = new Border
		{
			Child = label, Padding = new Thickness(18, 8), CornerRadius = new CornerRadius(10),
			Background = Brushes.Transparent, Cursor = new Cursor(StandardCursorType.Hand),
		};
		button.PointerEntered += (_, _) => button.Background = hover;
		button.PointerExited  += (_, _) => button.Background = Brushes.Transparent;
		button.PointerPressed += (_, _) => click();
		return button;
	}

	private void ShowChrome(bool show, bool pin = false)
	{
		_chromePinned = show && pin;
		if ((_chrome.Opacity > 0) == show) return;
		_chrome.Opacity          = show ? 1 : 0;
		_chrome.IsHitTestVisible = show;
	}

	private static Control Clickable(Control control, Action click)
	{
		var host = new Border { Child = control, Background = Brushes.Transparent, Cursor = new Cursor(StandardCursorType.Hand) };
		host.PointerPressed += (_, _) => click();
		return host;
	}

	private Border BuildPrompt()
	{
		var text = new StackPanel { Spacing = 26, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
		text.Children.Add(Text("¿Salir de Nova5?", 44, FontWeight.Bold));
		var keys = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 34, HorizontalAlignment = HorizontalAlignment.Center };
		keys.Children.Add(Clickable(Hint("A", "Salir"), Close));
		keys.Children.Add(Clickable(Hint("B", "Volver"), () => ShowPrompt(false)));
		text.Children.Add(keys);
		return new Border
		{
			Width = 1920, Height = 1080, IsVisible = false, Background = Palette.Brush(Palette.Night, 0.82), Child = text,
		};
	}

	// A titled panel in Nexo's style: "NOVA5 <version>" over the title, a close pill, the content and a key line.
	private static Border Sheet(string title, string keys, Control content, Action close, string closeLabel)
	{
		var heading = new StackPanel { Spacing = 2 };
		heading.Children.Add(new TextBlock
		{
			Text = $"NOVA5 {Updates.Current}", FontSize = 17, FontWeight = FontWeight.SemiBold, LetterSpacing = 1.5,
			Foreground = Palette.Brush(Palette.AuroraA),
		});
		heading.Children.Add(title.Length > 0 ? Text(title, 44, FontWeight.Black) : new Panel());
		var closer = Clickable(Pill(Text(closeLabel, 19, FontWeight.Bold, Palette.Muted)), close);
		closer.VerticalAlignment = VerticalAlignment.Top;
		var header = new DockPanel { Margin = new Thickness(0, 0, 0, 18) };
		DockPanel.SetDock(closer, Dock.Right);
		header.Children.Add(closer);
		header.Children.Add(heading);

		var footer = new TextBlock
		{
			Text = keys, FontSize = 18, Foreground = Palette.Brush(Palette.Muted), HorizontalAlignment = HorizontalAlignment.Center,
			Margin = new Thickness(0, 14, 0, 0),
		};
		var layout = new DockPanel();
		DockPanel.SetDock(header, Dock.Top);
		DockPanel.SetDock(footer, Dock.Bottom);
		layout.Children.Add(header);
		layout.Children.Add(footer);
		layout.Children.Add(content);
		return new Border
		{
			Child = layout, Padding = new Thickness(40, 30, 40, 22), CornerRadius = new CornerRadius(30),
			Background = Palette.Brush(Palette.Ink, 0.94), BorderBrush = Palette.Brush(Colors.White, 0.12), BorderThickness = new Thickness(1),
			BoxShadow = BoxShadows.Parse("0 30 80 0 #A0000000"),
		};
	}

	// ---- Release notes ------------------------------------------------------------------------

	// Shown once after an update (and from Settings > Información de la versión); A or B closes it.
	private Border BuildNotes()
	{
		_notesScroll.Content = _notesList;
		var content = new DockPanel();
		_notesTitle.Margin = new Thickness(0, 0, 0, 20);
		_notesTitle.TextWrapping = TextWrapping.Wrap;
		DockPanel.SetDock(_notesTitle, Dock.Top);
		content.Children.Add(_notesTitle);
		content.Children.Add(_notesScroll);
		var sheet = Sheet("", "▲ ▼ desplazar     A o B cerrar     Vuelve a verlas en Ajustes › Información de la versión", content, CloseNotes, "B  Cerrar");
		sheet.Width  = 1180;
		sheet.Height = 820;
		sheet.HorizontalAlignment = HorizontalAlignment.Center;
		sheet.VerticalAlignment   = VerticalAlignment.Center;
		sheet.PointerPressed += (_, e) => e.Handled = true; // Clicks inside do not reach the backdrop.
		var backdrop = new Border
		{
			Width = 1920, Height = 1080, IsVisible = false, Background = Palette.Brush(Palette.Night, 0.8), Child = sheet,
		};
		backdrop.PointerPressed += (_, _) => CloseNotes();
		return backdrop;
	}

	private void ShowNotes(List<ReleaseNote> notes, bool firstTime)
	{
		if (notes.Count == 0) return;
		_notesTitle.Text = firstTime ? $"Novedades de Nova5 {Updates.Current}" : "Información de la versión";
		_notesList.Children.Clear();
		foreach (var note in notes)
		{
			var block = new StackPanel { Spacing = 10 };
			var date  = DateTime.TryParse(note.Date, out var day) ? " · " + day.ToString("d 'de' MMMM 'de' yyyy", new System.Globalization.CultureInfo("es-ES")) : "";
			block.Children.Add(Text($"VERSIÓN {note.Version}{date}", 18, FontWeight.SemiBold, Palette.AuroraA));
			block.Children.Add(new TextBlock { Text = note.Title, FontSize = 30, FontWeight = FontWeight.Bold, Foreground = Palette.Brush(Palette.Text), TextWrapping = TextWrapping.Wrap });
			foreach (var item in note.Items)
			{
				var line = new DockPanel { Margin = new Thickness(0, 4, 0, 0) };
				var dot  = new Ellipse { Width = 9, Height = 9, Fill = Palette.Aurora(), VerticalAlignment = VerticalAlignment.Top, Margin = new Thickness(4, 12, 18, 0) };
				DockPanel.SetDock(dot, Dock.Left);
				line.Children.Add(dot);
				line.Children.Add(new TextBlock { Text = item, FontSize = 23, LineHeight = 33, Foreground = Palette.Brush(Palette.Text, 0.88), TextWrapping = TextWrapping.Wrap });
				block.Children.Add(line);
			}
			_notesList.Children.Add(block);
		}
		_notesScroll.Offset = default;
		if (_zone != Zone.Notes) _beforeNotes = _zone;
		_notes.IsVisible = true;
		_zone = Zone.Notes;
	}

	private void CloseNotes()
	{
		if (_zone != Zone.Notes) return;
		_notes.IsVisible = false;
		_zone = _beforeNotes;
		if (_store.SeenNotes != Updates.Current.ToString())
		{
			_store.SeenNotes = Updates.Current.ToString();
			_store.Save();
		}
	}

	private void NotesInput(PadInput input)
	{
		switch (input)
		{
			case PadInput.Up:   _notesScroll.Offset = _notesScroll.Offset.WithY(Math.Max(0, _notesScroll.Offset.Y - 140)); break;
			case PadInput.Down: _notesScroll.Offset = _notesScroll.Offset.WithY(_notesScroll.Offset.Y + 140); break;
			case PadInput.Accept or PadInput.Back: CloseNotes(); break;
		}
	}

	// ---- Keyboard and mouse -------------------------------------------------------------------

	// Each DualSense control with its key: A (or a click) waits for a key or mouse button, Supr clears, Esc cancels.
	private Border BuildKeys()
	{
		_keysScroll.Content = _keysList;
		var sheet = Sheet("Teclado y ratón", "▲ ▼ elegir     A asignar (pulsa una tecla o un botón del ratón)     Supr quitar     B cerrar", _keysScroll, CloseKeys, "B  Cerrar");
		sheet.Width  = 1320;
		sheet.Height = 860;
		sheet.HorizontalAlignment = HorizontalAlignment.Center;
		sheet.VerticalAlignment   = VerticalAlignment.Center;
		sheet.PointerPressed += (_, e) => e.Handled = true;
		// While a control waits, a click is the mouse button to give it.
		sheet.AddHandler(PointerPressedEvent, (_, e) =>
		{
			if (_capturing is null) return;
			var point = e.GetCurrentPoint(sheet).Properties;
			StopCapture(point.IsRightButtonPressed ? "Mouse:Right" : point.IsMiddleButtonPressed ? "Mouse:Middle"
			    : point.IsXButton1Pressed ? "Mouse:X1" : point.IsXButton2Pressed ? "Mouse:X2" : "Mouse:Left");
			e.Handled = true;
		}, Avalonia.Interactivity.RoutingStrategies.Tunnel);
		var backdrop = new Border { Width = 1920, Height = 1080, IsVisible = false, Background = Palette.Brush(Palette.Night, 0.8), Child = sheet };
		backdrop.PointerPressed += (_, _) => { if (_capturing is null) CloseKeys(); };
		return backdrop;
	}

	private void ShowKeys()
	{
		_keymap   = Keymap.Load(_emulator.Ini.Get("GlobalConfiguration", "keymap"));
		_keyIndex = 0;
		_capturing = null;
		_keysScroll.Offset = default;
		BuildKeyRows();
		_keysSheet.IsVisible = true;
		_zone = Zone.Keys;
	}

	private void CloseKeys()
	{
		if (_zone != Zone.Keys) return;
		_capturing = null;
		_keysSheet.IsVisible = false;
		_zone = Zone.Settings;
		BuildSettings();
	}

	private static readonly double[] Sensitivities = { 0.5, 0.75, 1, 1.25, 1.5, 2, 2.5, 3 };

	private List<Setting> KeyOptions()
	{
		var list = Keymap.Controls.Select(c => new Setting(c.Label, "", () => _capturing == c.Name ? "Pulsa una tecla…" : Keymap.Show(_keymap.GetValueOrDefault(c.Name, "")),
		    _ => { _capturing = c.Name; })).ToList();
		list.Add(new("Sensibilidad del ratón", "Para mover la cámara con el ratón: F7 lo activa dentro del juego.",
		    () => $"{Sensitivity():0.##}×", step =>
		    {
			    var now = Array.IndexOf(Sensitivities, Sensitivity());
			    SetIni("mouse_sensitivity", Sensitivities[(Math.Max(now, 2) + step + Sensitivities.Length) % Sensitivities.Length].ToString(System.Globalization.CultureInfo.InvariantCulture));
		    }));
		list.Add(new("Restablecer", "Vuelve a las teclas de Nova5 para todos los botones.", () => "Restablecer ›", _ =>
		{
			_keymap = new Dictionary<string, string>(Keymap.Defaults);
			SetIni("keymap", "");
			ShowToast("Teclas restablecidas");
		}));
		return list;
	}

	private double Sensitivity() =>
	    double.TryParse(_emulator.Ini.Get("GlobalConfiguration", "mouse_sensitivity"), System.Globalization.NumberStyles.Float,
	        System.Globalization.CultureInfo.InvariantCulture, out var value) ? value : 1;

	private void BuildKeyRows(bool reveal = false)
	{
		_keysList.Children.Clear();
		var options = KeyOptions();
		_keyIndex = Math.Clamp(_keyIndex, 0, options.Count - 1);
		for (var i = 0; i < options.Count; i++)
		{
			var index = i;
			_keysList.Children.Add(OptionRow(options[i], i == _keyIndex, step =>
			{
				if (_zone != Zone.Keys || _capturing is not null) return;
				_keyIndex = index;
				KeyOptions()[index].Change(step);
				BuildKeyRows();
			}));
		}
		if (reveal && _keysList.Children[_keyIndex] is Control chosen)
			Dispatcher.UIThread.Post(() => chosen.BringIntoView(), DispatcherPriority.Loaded);
	}

	// Gives the waiting control that input ("" clears it, null cancels); an input belongs to one control only.
	private void StopCapture(string? input)
	{
		if (_capturing is not { } control) return;
		_capturing = null;
		if (input is not null)
		{
			if (input.Length > 0)
				foreach (var other in _keymap.Where(m => m.Key != control && m.Value == input).Select(m => m.Key).ToList()) _keymap[other] = "";
			_keymap[control] = input;
			SetIni("keymap", Keymap.Save(_keymap));
		}
		BuildKeyRows();
	}

	private void KeysInput(PadInput input)
	{
		if (_capturing is not null)
		{
			if (input == PadInput.Back) StopCapture(null);
			return;
		}
		var options = KeyOptions();
		switch (input)
		{
			case PadInput.Up:   _keyIndex = Math.Max(0, _keyIndex - 1); break;
			case PadInput.Down: _keyIndex = Math.Min(options.Count - 1, _keyIndex + 1); break;
			case PadInput.Left: options[_keyIndex].Change(-1); break;
			case PadInput.Right or PadInput.Accept: options[_keyIndex].Change(1); break;
			case PadInput.Back: CloseKeys(); return;
		}
		BuildKeyRows(reveal: true);
	}

	// ---- Text entry ---------------------------------------------------------------------------

	// A one-line text box over everything (the keyboard types; Enter or A saves, Escape or B cancels).
	private Border BuildTextEntry()
	{
		var content = new StackPanel { Spacing = 22 };
		content.Children.Add(_textTitle);
		content.Children.Add(_textBox);
		var sheet = Sheet("", "Enter o A guardar     Esc o B cancelar", content, () => CloseText(false), "B  Cancelar");
		sheet.Width = 1000;
		sheet.HorizontalAlignment = HorizontalAlignment.Center;
		sheet.VerticalAlignment   = VerticalAlignment.Center;
		sheet.PointerPressed += (_, e) => e.Handled = true;
		return new Border { Width = 1920, Height = 1080, IsVisible = false, Background = Palette.Brush(Palette.Night, 0.8), Child = sheet };
	}

	private void AskText(string title, string current, Action<string> done)
	{
		_textTitle.Text = title;
		_textBox.Text   = current;
		_textDone       = done;
		_beforeText     = _zone;
		_zone           = Zone.TextEntry;
		_textSheet.IsVisible = true;
		Dispatcher.UIThread.Post(() => { _textBox.Focus(); _textBox.SelectAll(); }, DispatcherPriority.Loaded);
	}

	private void CloseText(bool save)
	{
		if (_zone != Zone.TextEntry) return;
		_textSheet.IsVisible = false;
		_zone = _beforeText;
		if (save && _textBox.Text?.Trim() is { Length: > 0 } text) _textDone?.Invoke(text);
		if (_zone == Zone.Settings) BuildSettings();
	}

	// ---- Trophies -----------------------------------------------------------------------------

	// Under the covers: the selected game's trophy progress and its latest trophies (read once per game; a game
	// that closes may have new ones).
	private void ShowGameTrophies(Game game)
	{
		if (!_trophyCache.TryGetValue(game.Dir, out var set))
		{
			set = Trophies.Load(_emulator.BaseDir, game, _emulator.Ini.Get("GlobalConfiguration", "user_id") ?? "1000",
			    _emulator.Ini.Get("GlobalConfiguration", "console_language") ?? "1");
			_trophyCache[game.Dir] = set;
		}
		_stripSet = set;
		_gameTrophies.IsVisible = set is not null;
		if (set is null) return;

		var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 22, VerticalAlignment = VerticalAlignment.Center };
		row.Children.Add(Cup(TrophyGrade.Platinum, 40));
		row.Children.Add(Text($"{set.Percent} %", 34, FontWeight.Bold));
		var summary = new StackPanel { Spacing = 8, VerticalAlignment = VerticalAlignment.Center };
		summary.Children.Add(Text($"{set.Earned} de {set.Trophies.Count} trofeos", 20, FontWeight.SemiBold, Palette.Muted));
		var bar = new Grid { Width = 220, Height = 6 };
		bar.Children.Add(new Border { CornerRadius = new CornerRadius(3), Background = Palette.Brush(Colors.White, 0.12) });
		bar.Children.Add(new Border { CornerRadius = new CornerRadius(3), Background = Palette.Aurora(), HorizontalAlignment = HorizontalAlignment.Left,
		                              Width = Math.Max(6, 220 * set.Percent / 100.0), IsVisible = set.Earned > 0 });
		summary.Children.Add(bar);
		row.Children.Add(summary);
		var counts = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 18, VerticalAlignment = VerticalAlignment.Center };
		FillCounts(counts, set.Count, null);
		row.Children.Add(counts);
		// The latest ones earned, newest first.
		foreach (var trophy in set.Trophies.Where(t => t.Unlocked is not null && t.Icon is not null).OrderByDescending(t => t.Unlocked).Take(5))
		{
			try
			{
				row.Children.Add(new Border
				{
					Width = 56, Height = 56, CornerRadius = new CornerRadius(10), ClipToBounds = true, VerticalAlignment = VerticalAlignment.Center,
					BorderBrush = Palette.Brush(GradeColor(trophy.Grade), 0.8), BorderThickness = new Thickness(2),
					Child = new Image { Source = new Bitmap(new MemoryStream(trophy.Icon!)), Stretch = Stretch.UniformToFill },
					[ToolTip.TipProperty] = trophy.Name,
				});
			}
			catch (Exception e) when (e is ArgumentException or NotSupportedException or InvalidOperationException)
			{
			}
		}
		_gameTrophies.Child = new Border
		{
			Child = row, Padding = new Thickness(22, 12), CornerRadius = new CornerRadius(20),
			Background = Palette.Brush(Palette.Ink, 0.62), BorderBrush = Palette.Brush(Colors.White, 0.1), BorderThickness = new Thickness(1),
		};
	}

	private static Color GradeColor(TrophyGrade grade) => grade switch
	{
		TrophyGrade.Platinum => Color.Parse("#A9DCF5"), TrophyGrade.Gold => Color.Parse("#F2C14E"),
		TrophyGrade.Silver => Color.Parse("#C8D0DA"), _ => Color.Parse("#D08A57"),
	};

	private static string GradeName(TrophyGrade grade) => grade switch
	{
		TrophyGrade.Platinum => "Platino", TrophyGrade.Gold => "Oro", TrophyGrade.Silver => "Plata", _ => "Bronce",
	};

	// A trophy cup in the grade's colour, drawn so it needs no emoji or icon font.
	private static Control Cup(TrophyGrade grade, double size) => new Viewbox
	{
		Width = size, Height = size, VerticalAlignment = VerticalAlignment.Center,
		Child = new Avalonia.Controls.Shapes.Path
		{
			Data = Geometry.Parse("M7,2 H17 V4 H21 V7 C21,9.8 19,11.6 16.4,11.9 C15.6,13.6 14.1,14.8 13,15.1 V18 H16 V21 H8 V18 H11 V15.1 C9.9,14.8 8.4,13.6 7.6,11.9 C5,11.6 3,9.8 3,7 V4 H7 Z M5,6 V7 C5,8.3 5.8,9.4 7,9.8 V6 Z M17,6 V9.8 C18.2,9.4 19,8.3 19,7 V6 Z"),
			Fill = Palette.Brush(GradeColor(grade)),
		},
	};

	// "🏆 3/12" counters for each grade, with an optional total in front.
	private static void FillCounts(StackPanel panel, Func<TrophyGrade, (int Earned, int Total)> count, string? lead)
	{
		panel.Children.Clear();
		if (lead is not null) panel.Children.Add(Text(lead, 22, FontWeight.SemiBold));
		foreach (var grade in Enum.GetValues<TrophyGrade>())
		{
			var (earned, total) = count(grade);
			if (total == 0) continue;
			var item = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
			item.Children.Add(Cup(grade, 26));
			item.Children.Add(Text($"{earned}/{total}", 20, FontWeight.SemiBold, Palette.Muted));
			panel.Children.Add(item);
		}
	}

	private void BuildTrophies(bool reveal = false)
	{
		if (!reveal)
		{
			var user     = _emulator.Ini.Get("GlobalConfiguration", "user_id") ?? "1000";
			var language = _emulator.Ini.Get("GlobalConfiguration", "console_language") ?? "1";
			_trophySets = _games.Where(g => g.Package is null)
			    .Select(g => Trophies.Load(_emulator.BaseDir, g, user, language)).OfType<TrophySet>()
			    .OrderByDescending(s => s.LastUnlock ?? DateTime.MinValue).ThenBy(s => s.Title).ToList();
		}
		var all = _trophySets.SelectMany(s => s.Trophies).ToList();
		FillCounts(_trophySummary, grade => (all.Count(t => t.Grade == grade && t.Unlocked is not null), all.Count(t => t.Grade == grade)),
		    $"{all.Count(t => t.Unlocked is not null)} de {all.Count} trofeos en {_trophySets.Count} juego(s)");
		_trophyGames.Children.Clear();
		if (_trophySets.Count == 0)
		{
			_trophyGames.Children.Add(new TextBlock
			{
				Text = "Ningún juego de tu biblioteca trae lista de trofeos (sce_sys/trophy2).", FontSize = 22,
				Foreground = Palette.Brush(Palette.Muted), TextWrapping = TextWrapping.Wrap, Margin = new Thickness(26, 10),
			});
			return;
		}
		_trophyIndex = Math.Clamp(_trophyIndex, 0, _trophySets.Count - 1);
		for (var i = 0; i < _trophySets.Count; i++)
		{
			var set   = _trophySets[i];
			var index = i;
			var row   = TrophyGameRow(set, i == _trophyIndex);
			row.PointerPressed += (_, _) =>
			{
				if (_zone != Zone.Trophies) return;
				_trophyIndex = index;
				ShowTrophyList(set);
			};
			_trophyGames.Children.Add(row);
		}
		if (reveal && _trophyGames.Children[_trophyIndex] is Control chosen)
			Dispatcher.UIThread.Post(() => chosen.BringIntoView(), DispatcherPriority.Loaded);
	}

	private Border TrophyGameRow(TrophySet set, bool focused)
	{
		var icon = new Border { Width = 92, Height = 92, CornerRadius = new CornerRadius(14), ClipToBounds = true, Background = Palette.Brush(Palette.Ink) };
		if (set.Game.Icon is { } path)
		{
			try
			{
				using var stream = File.OpenRead(path);
				icon.Child = new Image { Source = Bitmap.DecodeToWidth(stream, 184), Stretch = Stretch.UniformToFill };
			}
			catch (Exception e) when (e is IOException or ArgumentException or NotSupportedException or InvalidOperationException)
			{
			}
		}
		var counts = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 22 };
		FillCounts(counts, set.Count, null);
		var bar = new Grid { Height = 8, Margin = new Thickness(0, 4, 0, 0) };
		bar.Children.Add(new Border { CornerRadius = new CornerRadius(4), Background = Palette.Brush(Colors.White, 0.1) });
		bar.Children.Add(new Border
		{
			CornerRadius = new CornerRadius(4), Background = Palette.Aurora(), HorizontalAlignment = HorizontalAlignment.Left,
			Width = Math.Max(8, 760 * set.Percent / 100.0), IsVisible = set.Earned > 0,
		});
		var text = new StackPanel { Spacing = 6, VerticalAlignment = VerticalAlignment.Center, HorizontalAlignment = HorizontalAlignment.Left, Margin = new Thickness(24, 0, 0, 0), Width = 760 };
		text.Children.Add(new TextBlock { Text = set.Title, FontSize = 25, FontWeight = FontWeight.SemiBold, Foreground = Palette.Brush(Palette.Text), TextTrimming = TextTrimming.CharacterEllipsis });
		text.Children.Add(Text($"{set.Earned} de {set.Trophies.Count} conseguidos" + (set.LastUnlock is { } last && last > DateTime.MinValue ? $" · último el {last:d MMM yyyy}" : ""), 18, FontWeight.Normal, Palette.Muted));
		text.Children.Add(counts);
		text.Children.Add(bar);
		var percent = new Border
		{
			Padding = new Thickness(18, 7), CornerRadius = new CornerRadius(999), MinWidth = 86, VerticalAlignment = VerticalAlignment.Center,
			Background = set.Percent == 100 ? Palette.Aurora() : Palette.Brush(Colors.White, 0.08),
			Child = new TextBlock { Text = $"{set.Percent} %", FontSize = 20, FontWeight = FontWeight.Bold, HorizontalAlignment = HorizontalAlignment.Center,
			                        Foreground = set.Percent == 100 ? Palette.Brush(Color.Parse("#081022")) : Palette.Brush(Palette.Text, 0.9) },
		};
		var row = new DockPanel();
		DockPanel.SetDock(icon, Dock.Left);
		DockPanel.SetDock(percent, Dock.Right);
		row.Children.Add(icon);
		row.Children.Add(percent);
		row.Children.Add(text);
		return new Border
		{
			Child = row, Padding = new Thickness(20, 14), CornerRadius = new CornerRadius(18), Margin = new Thickness(0, 0, 14, 0),
			Background = focused ? Palette.Brush(Colors.White, 0.09) : Brushes.Transparent,
			BorderBrush = focused ? Palette.Aurora() : Brushes.Transparent, BorderThickness = new Thickness(2),
			Cursor = new Cursor(StandardCursorType.Hand),
		};
	}

	private void TrophiesInput(PadInput input)
	{
		switch (input)
		{
			case PadInput.Up:   _trophyIndex = Math.Max(0, _trophyIndex - 1); break;
			case PadInput.Down: _trophyIndex = Math.Min(Math.Max(0, _trophySets.Count - 1), _trophyIndex + 1); break;
			case PadInput.Accept when _trophyIndex < _trophySets.Count: ShowTrophyList(_trophySets[_trophyIndex]); return;
			case PadInput.Back: ShowTab(0); return;
		}
		BuildTrophies(reveal: true);
	}

	// A click on the selected game's trophy strip: the Trophies tab with that game's list open.
	private void OpenStripTrophies()
	{
		if (_stripSet is not { } set || _zone is not (Zone.Tiles or Zone.Buttons) || _running is not null) return;
		ShowTab(TrophiesTab);
		_trophyIndex = Math.Max(0, _trophySets.FindIndex(s => string.Equals(s.Game.Dir, set.Game.Dir, StringComparison.OrdinalIgnoreCase)));
		BuildTrophies(reveal: true);
		ShowTrophyList(_trophySets.ElementAtOrDefault(_trophyIndex) ?? set);
	}

	// One game's trophies: icon, grade, name and description; hidden ones stay hidden until earned.
	private Border BuildTrophySheet()
	{
		_trophyListScroll.Content = _trophyList;
		var content = new DockPanel();
		_trophyListTitle.TextWrapping = TextWrapping.Wrap;
		_trophyListTitle.MaxLines = 2;
		_trophyListTitle.Margin = new Thickness(0, 0, 0, 10);
		DockPanel.SetDock(_trophyListTitle, Dock.Top);
		DockPanel.SetDock(_trophyListSummary, Dock.Top);
		content.Children.Add(_trophyListTitle);
		content.Children.Add(_trophyListSummary);
		content.Children.Add(_trophyListScroll);
		var sheet = Sheet("", "▲ ▼ desplazar     A o B cerrar", content, CloseTrophyList, "B  Cerrar");
		sheet.Width  = 1320;
		sheet.Height = 860;
		sheet.HorizontalAlignment = HorizontalAlignment.Center;
		sheet.VerticalAlignment   = VerticalAlignment.Center;
		sheet.PointerPressed += (_, e) => e.Handled = true;
		var backdrop = new Border { Width = 1920, Height = 1080, IsVisible = false, Background = Palette.Brush(Palette.Night, 0.8), Child = sheet };
		backdrop.PointerPressed += (_, _) => CloseTrophyList();
		return backdrop;
	}

	private void ShowTrophyList(TrophySet set)
	{
		_trophyListTitle.Text = set.Title;
		FillCounts(_trophyListSummary, set.Count, $"{set.Earned} de {set.Trophies.Count} · {set.Percent} %");
		_trophyList.Children.Clear();
		foreach (var trophy in set.Trophies.OrderBy(t => t.Unlocked is null).ThenBy(t => t.Grade).ThenBy(t => t.Id))
		{
			var earned = trophy.Unlocked is not null;
			var secret = trophy.Hidden && !earned;
			var icon = new Border { Width = 78, Height = 78, CornerRadius = new CornerRadius(12), ClipToBounds = true, Background = Palette.Brush(Palette.Ink), Opacity = earned ? 1 : 0.35 };
			if (!secret && trophy.Icon is { } png)
			{
				try
				{
					icon.Child = new Image { Source = new Bitmap(new MemoryStream(png)), Stretch = Stretch.UniformToFill };
				}
				catch (Exception e) when (e is ArgumentException or NotSupportedException or InvalidOperationException)
				{
				}
			}
			if (icon.Child is null) icon.Child = new Panel { Children = { new Viewbox { Margin = new Thickness(18), Child = Cup(trophy.Grade, 40) } } };
			var text = new StackPanel { Spacing = 3, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(22, 0, 0, 0) };
			text.Children.Add(new TextBlock { Text = secret ? "Trofeo oculto" : trophy.Name, FontSize = 24, FontWeight = FontWeight.SemiBold, Foreground = Palette.Brush(Palette.Text, earned ? 1 : 0.75), TextWrapping = TextWrapping.Wrap });
			text.Children.Add(new TextBlock { Text = secret ? "Sigue jugando para descubrirlo." : trophy.Description, FontSize = 18, Foreground = Palette.Brush(Palette.Muted), TextWrapping = TextWrapping.Wrap });
			var state = new StackPanel { Spacing = 4, VerticalAlignment = VerticalAlignment.Center, HorizontalAlignment = HorizontalAlignment.Right, Margin = new Thickness(24, 0, 10, 0) };
			var grade = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, HorizontalAlignment = HorizontalAlignment.Right };
			grade.Children.Add(Cup(trophy.Grade, 24));
			grade.Children.Add(Text(GradeName(trophy.Grade), 19, FontWeight.SemiBold, GradeColor(trophy.Grade)));
			state.Children.Add(grade);
			state.Children.Add(new TextBlock
			{
				Text = !earned ? "Bloqueado" : trophy.Unlocked > DateTime.MinValue ? $"Conseguido el {trophy.Unlocked:d MMM yyyy}" : "Conseguido",
				FontSize = 17, Foreground = Palette.Brush(earned ? Palette.AuroraA : Palette.Muted), HorizontalAlignment = HorizontalAlignment.Right,
			});
			var row = new DockPanel();
			DockPanel.SetDock(icon, Dock.Left);
			DockPanel.SetDock(state, Dock.Right);
			row.Children.Add(icon);
			row.Children.Add(state);
			row.Children.Add(text);
			_trophyList.Children.Add(new Border { Child = row, Padding = new Thickness(16, 10), CornerRadius = new CornerRadius(16), Margin = new Thickness(0, 0, 14, 0),
			                                      Background = earned ? Palette.Brush(Colors.White, 0.05) : Brushes.Transparent });
		}
		_trophyListScroll.Offset = default;
		if (_trophyPanel is not null) _trophyPanel.IsVisible = false;
		_trophySheet.IsVisible = true;
		_zone = Zone.TrophyList;
	}

	private void CloseTrophyList()
	{
		if (_zone != Zone.TrophyList) return;
		_trophySheet.IsVisible = false;
		if (_trophyPanel is not null) _trophyPanel.IsVisible = true;
		_zone = Zone.Trophies;
	}

	private void TrophyListInput(PadInput input)
	{
		switch (input)
		{
			case PadInput.Up:   _trophyListScroll.Offset = _trophyListScroll.Offset.WithY(Math.Max(0, _trophyListScroll.Offset.Y - 140)); break;
			case PadInput.Down: _trophyListScroll.Offset = _trophyListScroll.Offset.WithY(_trophyListScroll.Offset.Y + 140); break;
			case PadInput.Accept or PadInput.Back: CloseTrophyList(); break;
		}
	}

	// ---- Game options -------------------------------------------------------------------------

	// A game's own options (its button or a right click on it): performance mode, each patch, community
	// patches, folder and title ID.
	private Border BuildGameOptions()
	{
		_gameScroll.Content = _gameList;
		var content = new DockPanel();
		_gameTitle.Margin = new Thickness(0, 0, 0, 20);
		_gameTitle.TextWrapping = TextWrapping.Wrap;
		_gameTitle.MaxLines = 2;
		DockPanel.SetDock(_gameTitle, Dock.Top);
		content.Children.Add(_gameTitle);
		content.Children.Add(_gameScroll);
		var sheet = Sheet("", "▲ ▼ elegir     A ▸ cambiar     ◂ valor anterior     B cerrar", content, CloseGameOptions, "B  Cerrar");
		sheet.Width  = 1320;
		sheet.Height = 860;
		sheet.HorizontalAlignment = HorizontalAlignment.Center;
		sheet.VerticalAlignment   = VerticalAlignment.Center;
		sheet.PointerPressed += (_, e) => e.Handled = true;
		var backdrop = new Border
		{
			Width = 1920, Height = 1080, IsVisible = false, Background = Palette.Brush(Palette.Night, 0.8), Child = sheet,
		};
		backdrop.PointerPressed += (_, _) => CloseGameOptions();
		return backdrop;
	}

	private void ShowGameOptions(Game game)
	{
		if (game.Package is not null) return;
		_optionsGame = game;
		_patches     = GamePatches.Load(_emulator.BaseDir, game);
		_gameOption  = 0;
		_gameTitle.Text = game.Name;
		_gameScroll.Offset = default;
		BuildGameRows();
		_gameSheet.IsVisible = true;
		_zone = Zone.GameOptions;
	}

	private void CloseGameOptions()
	{
		if (_zone != Zone.GameOptions) return;
		_gameSheet.IsVisible = false;
		_zone = Zone.Buttons;
		SetZoneVisuals();
		Select(_index, force: true); // The "Parche activo" badge may have changed.
	}

	private static string ModeLabel(string mode) => mode switch
	{
		"calidad" => "Calidad", "equilibrado" => "Equilibrado", "rendimiento" => "Rendimiento", _ => "Personalizado",
	};

	private List<Setting> GameOptions()
	{
		var game = _optionsGame!;
		var list = new List<Setting> { Section("Parches") };
		if (_patches is { HasModes: true } patches)
		{
			list.Add(new("Modo", "Calidad: mejor imagen. Rendimiento: más FPS quitando efectos. Equilibrado: entre los dos. Personalizado: lo que elijas abajo.",
			    () => ModeLabel(patches.Mode), step =>
			    {
				    var modes = GamePatches.ModeNames;
				    var now   = Array.IndexOf(modes, patches.Mode);
				    patches.ApplyMode(modes[now < 0 ? (step > 0 ? 0 : modes.Length - 1) : (now + step + modes.Length) % modes.Length]);
			    }));
		}
		if (_patches is { } file)
		{
			foreach (var patch in file.Mods)
			{
				var index = patch.Index;
				list.Add(new(patch.Label, patch.Detail, () => file.Mods[index].Enabled ? "Sí" : "No",
				    _ => file.Set(index, !file.Mods[index].Enabled)));
			}
		}
		else
		{
			list.Add(new("Sin parches", $"No hay parches para la versión {game.Version} de este juego. Puedes buscar los de la comunidad.", () => "", _ => { }));
		}
		list.Add(new("Buscar parches de la comunidad",
		    $"Descarga los parches publicados para la versión {game.Version} (colección HEN-Cheats-Collection en GitHub). Llegan desactivados.",
		    () => _downloadingPatches ? "Buscando…" : "Buscar ›", _ => DownloadPatches(game)));
		// This game's own settings ([<title id>] in Nova5.ini), in the general settings' sections; "General" follows those.
		var id = game.TitleId;
		list.Add(Section("Gráficos"));
		list.Add(GameCycle(id, "Resolución", "Resolución a la que se dibuja este juego.", "screen_resolution", "R1920X1080",
		    Resolutions.Select(r => (r, r.TrimStart('R').Replace('X', '×'))).ToArray()));
		// Games with dynamic resolution (Astro Bot) pick it from the GPU time they measure; a smaller time lets them draw more.
		list.Add(GameCycle(id, "Resolución dinámica", "Para juegos que bajan su resolución solos, como Astro Bot: más nítido dibuja más píxeles y puede bajar algo los FPS.",
		    "gpu_time_scale", "100", ("100", "Normal"), ("75", "Más nítida"), ("50", "Mucho más nítida"), ("33", "Máxima")));
		list.Add(GameCycle(id, "Frecuencia de pantalla", "120 o 144 Hz si este juego tiene modo de 120 Hz; 60 Hz si va acelerado.", "vblank_frequency", "60",
		    RefreshRates.Select(hz => (hz.ToString(), $"{hz} Hz")).ToArray()));
		list.Add(GameCycle(id, "Sincronización (VSync)", "Mailbox: fluido. VSync: sin cortes, algo más de retraso. Sin límite: el menor retraso.", "present_mode", "Mailbox",
		    ("Mailbox", "Mailbox"), ("Fifo", "VSync"), ("Immediate", "Sin límite")));
		list.Add(GameToggle(id, "HDR", "Imagen HDR si tu monitor y el juego lo admiten.", "hdr_enabled", false));
		list.Add(GameToggle(id, "Pantalla completa", "Este juego se abre a pantalla completa o en una ventana.", "fullscreen_enabled", true));
		list.Add(GameToggle(id, "Contador de FPS", "Muestra los FPS mientras juegas a este juego.", "perf_overlay_enabled", false));
		list.Add(GameToggle(id, "Ocultar el cursor", "Oculta el puntero del ratón dentro de este juego.", "hide_cursor_enabled", false));
		list.Add(Section("Mando"));
		list.Add(GameCycle(id, "Vibración del mando", "Intensidad de la vibración en este juego (0 % la apaga).", "controller_vibration_intensity", "100",
		    ("100", "100 %"), ("75", "75 %"), ("50", "50 %"), ("25", "25 %"), ("0", "Apagada")));
		list.Add(GameCycle(id, "Altavoz del mando", "Volumen del altavoz del DualSense en este juego.", "controller_speaker_volume", "50",
		    ("100", "100 %"), ("75", "75 %"), ("50", "50 %"), ("25", "25 %"), ("0", "Apagado")));
		list.Add(GameCycle(id, "Color de la barra de luz", "Color de la luz del DualSense en este juego. Automático: el que pida el juego.", "controller_color", "",
		    ("", "Automático"), ("#0050FF", "Azul"), ("#5EEAD4", "Aurora"), ("#8B5CF6", "Morado"), ("#FF2D55", "Rojo"), ("#30D158", "Verde"), ("#FFFFFF", "Blanco")));
		list.Add(Section("Sistema"));
		list.Add(GameCycle(id, "Idioma de la consola", "El idioma de textos y voces de este juego, si lo tiene.", "console_language", "1", ConsoleLanguages));
		list.Add(GameToggle(id, "Saltar avisos de inicio", "Pide al juego que se salte los avisos del principio, si lo permite.", "skip_notice_screen", false));
		list.Add(Section("Rendimiento"));
		list.Add(GameToggle(id, "Caché de shaders", "Guarda los shaders de este juego para que cargue más rápido.", "shader_cache_enabled", true));
		list.Add(GameToggle(id, "Compatibilidad de CPU", "Solo si este juego se cierra al arrancar en tu procesador.", "amd_cpu_enabled", false));
		// Options that only apply to this game.
		if (game.TitleId == "PPSA01341")
		{
			list.AddRange(new[]
			{
			DemonsSoulsToggle("Espera de los hilos de trabajo", "Los hilos de trabajo del juego esperan sin gastar procesador. Experimental.", "ds_idle_enabled"),
			DemonsSoulsToggle("Pausa en bucles de espera", "Menos uso del procesador mientras el juego espera. Experimental.", "ds_spin_backoff_enabled"),
			DemonsSoulsToggle("Sin descarte de índices", "Omite un paso de descarte de geometría que el emulador no necesita. Experimental.", "ds_disable_index_culling"),
			DemonsSoulsToggle("Núcleos de rendimiento", "Coloca los hilos de dibujo en los núcleos rápidos del procesador. Experimental.", "ds_affinity_enabled"),
			DemonsSoulsToggle("Prioridad de dibujo", "Da más prioridad al hilo de dibujo. Experimental.", "ds_render_priority_enabled"),
			});
		}
		list.Add(Section("Archivos del juego"));
		// Additional content: what the emulator will load (installed DLC packages of this game).
		var addons = Library.Addons(_emulator.GameDirs().Concat(_store.ExtraDirs), game.TitleId);
		list.Add(addons.Count == 0
		    ? new("Contenido adicional (DLC)", "No hay DLC instalado. Copia el .pkg del DLC en tu carpeta de juegos y Nova5 lo instalará solo, como los juegos.", () => "", _ => { })
		    : new($"Contenido adicional (DLC): {addons.Count}", string.Join(" · ", addons.Select(a => a.Name)), () => "Instalado", _ => { }));
		if (game.Archive is null)
		{
			var packed = GameArchive.Packed(game);
			if (packed is null)
			{
				list.Add(new("Comprimir en .zar",
				    "Guarda el juego en un solo archivo comprimido que ocupa mucho menos (Astro Bot pasa de 148 a 44 GB). Se juega igual, sin descomprimir. La carpeta original no se toca.",
				    () => _packing == game ? $"Comprimiendo {_packProgress * 100:0} %" : _packing is not null ? "Espera…" : "Comprimir ›", _ => PackGame(game)));
			}
			else
			{
				list.Add(new("Borrar la carpeta original",
				    $"Ya está comprimido en {System.IO.Path.GetFileName(packed)}: lleva la carpeta a la papelera para liberar espacio. El juego comprimido aparece en la biblioteca.",
				    () => _confirmDelete == game.Dir ? "¿Seguro? Pulsa otra vez" : "Borrar ›", _ =>
				    {
					    if (_confirmDelete != game.Dir)
					    {
						    _confirmDelete = game.Dir;
						    return;
					    }
					    _confirmDelete = null;
					    try
					    {
						    GameArchive.RecycleFolder(game.Dir);
						    CloseGameOptions();
						    Reload();
						    ShowToast("Carpeta original en la papelera");
					    }
					    catch (Exception e) when (e is IOException or UnauthorizedAccessException or OperationCanceledException)
					    {
						    ShowToast("No se pudo borrar: " + e.Message);
					    }
				    }));
			}
		}
		list.Add(new("Abrir la carpeta del juego", game.Dir, () => "Abrir ›",
		    _ => Process.Start(new ProcessStartInfo(game.Dir) { UseShellExecute = true })));
		list.Add(new("Copiar el ID del título", $"{game.TitleId} · versión {game.Version}", () => "Copiar ›", _ =>
		{
			Clipboard?.SetTextAsync(game.TitleId);
			ShowToast($"{game.TitleId} copiado");
		}));
		return list;
	}

	private void BuildGameRows(bool reveal = false)
	{
		_gameList.Children.Clear();
		var options = GameOptions();
		_gameOption = Math.Clamp(_gameOption, 0, options.Count - 1);
		while (options[_gameOption].Header && _gameOption < options.Count - 1) _gameOption++;
		for (var i = 0; i < options.Count; i++)
		{
			var index = i;
			if (options[i].Header)
			{
				_gameList.Children.Add(SectionTitle(options[i].Label));
				continue;
			}
			_gameList.Children.Add(OptionRow(options[i], i == _gameOption, step =>
			{
				if (_zone != Zone.GameOptions) return;
				_gameOption = index;
				GameOptions()[index].Change(step);
				if (_zone == Zone.GameOptions) BuildGameRows();
			}));
		}
		if (reveal && _gameList.Children[_gameOption] is Control chosen)
			Dispatcher.UIThread.Post(() => chosen.BringIntoView(), DispatcherPriority.Loaded);
	}

	private void GameOptionsInput(PadInput input)
	{
		var options = GameOptions();
		switch (input)
		{
			case PadInput.Up:   _gameOption = Step(options, _gameOption, -1); break;
			case PadInput.Down: _gameOption = Step(options, _gameOption, 1); break;
			case PadInput.Left: options[_gameOption].Change(-1); break;
			case PadInput.Right or PadInput.Accept: options[_gameOption].Change(1); break;
			case PadInput.Back: CloseGameOptions(); return;
		}
		if (_zone == Zone.GameOptions) BuildGameRows(reveal: true);
	}

	// Packs a game folder into .zar in the background: the progress (and time left) stays in the top bar, and at
	// the end the player chooses what to do with the original folder.
	private async void PackGame(Game game)
	{
		if (_packing is not null)
		{
			ShowToast($"Ya se está comprimiendo {_packing.Name}");
			return;
		}
		if (_running is not null)
		{
			ShowToast("Cierra el juego antes de comprimirlo");
			return;
		}
		_packing      = game;
		_packProgress = 0;
		_packCancel   = new CancellationTokenSource();
		_packText.Text      = $"Comprimiendo {game.Name}: preparando…";
		_packPill.IsVisible = true;
		if (_zone == Zone.GameOptions) BuildGameRows();
		ShowToast($"Comprimiendo {game.Name} en .zar. Puedes seguir usando Nova5; no lo cierres hasta que termine.");
		var started = DateTime.UtcNow;
		var shown   = DateTime.MinValue;
		var size    = 0L;
		try
		{
			// The UI follows at most twice a second (a game has hundreds of thousands of files).
			var progress = new Progress<double>(p =>
			{
				_packProgress = p;
				if ((DateTime.UtcNow - shown).TotalMilliseconds < 500 && p < 1) return;
				shown = DateTime.UtcNow;
				var elapsed = DateTime.UtcNow - started;
				var left    = p > 0.02 ? TimeSpan.FromSeconds(elapsed.TotalSeconds * (1 - p) / p) : (TimeSpan?)null;
				_packText.Text = $"Comprimiendo {game.Name}: {p * 100:0} %" + (left is { } l ? $" · quedan {Math.Max(1, Math.Round(l.TotalMinutes))} min" : "");
				if (_zone == Zone.GameOptions && _optionsGame == game) BuildGameRows();
			});
			var token   = _packCancel.Token;
			size        = await Task.Run(() => GameArchive.FolderSize(game.Dir), token);
			var archive = await Task.Run(() => GameArchive.PackAsync(game, progress, token), token);
			_packing = null;
			_packPill.IsVisible = false;
			Reload();
			AfterPack(game, archive, size);
		}
		catch (OperationCanceledException)
		{
			ShowToast("Compresión cancelada; el juego sigue como estaba");
		}
		catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.ComponentModel.Win32Exception)
		{
			ShowToast("No se pudo comprimir: " + e.Message);
		}
		finally
		{
			_packing = null;
			_packPill.IsVisible = false;
			_packCancel?.Dispose();
			_packCancel = null;
			if (_zone == Zone.GameOptions) BuildGameRows();
		}
	}

	private void AskPrep(Game game)
	{
		if (_prepping is not null)
		{
			ShowToast($"Ya se están preparando los shaders de {_prepping.Name}");
			return;
		}
		ShowChoice("Preparar shaders", $"Nova5 compila ahora los shaders de {game.Name} para que no se trabe al llegar a zonas nuevas. Puedes seguir usando el PC; va con prioridad baja.", new()
		{
			new("Rápido", "Lo que ya has jugado y lo más usado del juego. Más o menos la mitad del tiempo.", () => "", _ => PrepareShaders(game, quick: true)),
			new("Completo", "Todos los shaders del juego. Puede tardar horas la primera vez: ideal para dejarlo de noche.", () => "", _ => PrepareShaders(game, quick: false)),
			new("Ahora no", "", () => "", _ => { }),
		});
	}

	private async void PrepareShaders(Game game, bool quick)
	{
		if (_prepping is not null) return;
		_prepping     = game;
		_prepProgress = 0;
		_prepReady    = false;
		_prepCancel   = new CancellationTokenSource();
		_prepText.Text      = $"Shaders de {game.Name}: empezando…";
		_prepPill.IsVisible = true;
		if (_zone == Zone.GameOptions) BuildGameRows();
		ShowToast($"Preparando los shaders de {game.Name}. Puedes seguir usando Nova5; no lo cierres hasta que termine.");
		var started = DateTime.UtcNow;
		try
		{
			var progress = new Progress<(double Done, bool Ready)>(p =>
			{
				if (p.Done >= 0) _prepProgress = p.Done;
				_prepReady = p.Ready;
				var elapsed = DateTime.UtcNow - started;
				var left    = _prepProgress > 0.03 ? TimeSpan.FromSeconds(elapsed.TotalSeconds * (1 - _prepProgress) / _prepProgress) : (TimeSpan?)null;
				_prepText.Text = $"Shaders de {game.Name}: {_prepProgress * 100:0.0} %" + (_prepReady ? " · ya se puede jugar" : "") +
				                 (left is { } l ? $" · quedan {(l.TotalHours >= 1 ? $"{l.TotalHours:0.#} h" : $"{Math.Max(1, Math.Round(l.TotalMinutes))} min")}" : "");
				if (_zone == Zone.GameOptions && _optionsGame == game) BuildGameRows();
			});
			ShowToast(await ShaderPrep.RunAsync(game, quick, progress, _prepCancel.Token)
			    ? $"Shaders de {game.Name} preparados: ya no debería trabarse al llegar a zonas nuevas"
			    : $"{game.Name} no tiene shaders que preparar por adelantado: los compila el juego al usarlos");
		}
		catch (OperationCanceledException)
		{
			ShowToast("Preparación de shaders cancelada; lo ya preparado se conserva");
		}
		catch (Exception e)
		{
			// In a box that stays: the run may have gone on for hours. What the script said is in logs\preparar-shaders-<id>.txt.
			ShowChoice("No se pudieron preparar los shaders", $"{e.Message}\n\nLo ya preparado se conserva. El detalle está en logs\\preparar-shaders-{game.TitleId}.txt.",
			    new() { new("Aceptar", "", () => "", _ => { }) });
		}
		finally
		{
			_prepping = null;
			_prepPill.IsVisible = false;
			_prepCancel?.Dispose();
			_prepCancel = null;
			if (_zone == Zone.GameOptions) BuildGameRows();
		}
	}

	private void AskCancelPrep()
	{
		if (_prepping is not { } game || _zone is Zone.Choice or Zone.TextEntry) return;
		ShowChoice("¿Cancelar la preparación de shaders?", $"Se están preparando los shaders de {game.Name} ({_prepProgress * 100:0.0} %). Lo ya preparado se conserva y la próxima vez sigue desde ahí.", new()
		{
			new("Seguir preparando", "", () => "", _ => { }),
			new("Cancelar", "", () => "", _ => _prepCancel?.Cancel()),
		});
	}

	private void AskCancelPack()
	{
		if (_packing is not { } game || _zone is Zone.Choice or Zone.TextEntry) return;
		ShowChoice("¿Cancelar la compresión?", $"Se está comprimiendo {game.Name} ({_packProgress * 100:0} %). Si cancelas se borra el .zar a medias y el juego queda como estaba.", new()
		{
			new("Seguir comprimiendo", "", () => "", _ => { }),
			new("Cancelar la compresión", "", () => "", _ => _packCancel?.Cancel()),
		});
	}

	// The .zar is ready: delete the folder now, try the .zar first (asked again when it closes), or later.
	private void AfterPack(Game folder, string archive, long folderSize)
	{
		var packedSize = new FileInfo(archive).Length;
		ShowChoice($"{folder.Name} ya está comprimido",
		    $"El .zar ocupa {packedSize / 1e9:0.0} GB (la carpeta, {folderSize / 1e9:0.0} GB). ¿Qué hacemos con la carpeta original?", new()
		{
			new("Probar primero el .zar", "Abre ahora el juego comprimido. Al cerrarlo te pregunto si funcionó bien antes de borrar nada.",
			    () => "", _ => TryArchive(folder.Dir, archive)),
			new("Borrar la carpeta y quedarme con el .zar", $"La carpeta va a la papelera y liberas unos {folderSize / 1e9:0} GB.",
			    () => "", _ => RecycleOriginal(folder.Dir)),
			new("Decidir más tarde", "Se quedan las dos versiones. Puedes borrar la carpeta luego con clic derecho en el juego.",
			    () => "", _ => { }),
		});
	}

	private void TryArchive(string folder, string archive)
	{
		if (_games.FirstOrDefault(g => string.Equals(g.Archive, archive, StringComparison.OrdinalIgnoreCase)) is not { } packed)
		{
			ShowToast("No encuentro el juego comprimido en la biblioteca");
			return;
		}
		_zarTest = (folder, archive);
		Select(_games.IndexOf(packed), force: true);
		Launch(packed);
	}

	private void RecycleOriginal(string folder)
	{
		try
		{
			GameArchive.RecycleFolder(folder);
			Reload();
			ShowToast("Carpeta original en la papelera; te queda el .zar");
		}
		catch (Exception e) when (e is IOException or UnauthorizedAccessException or OperationCanceledException)
		{
			ShowToast("No se pudo borrar la carpeta: " + e.Message);
		}
	}

	private void RecycleArchive(string archive)
	{
		try
		{
			Microsoft.VisualBasic.FileIO.FileSystem.DeleteFile(archive, Microsoft.VisualBasic.FileIO.UIOption.OnlyErrorDialogs,
			    Microsoft.VisualBasic.FileIO.RecycleOption.SendToRecycleBin);
			if (Directory.Exists(GameArchive.Meta(archive))) GameArchive.RecycleFolder(GameArchive.Meta(archive));
			Reload();
			ShowToast(".zar en la papelera; te queda la carpeta");
		}
		catch (Exception e) when (e is IOException or UnauthorizedAccessException or OperationCanceledException)
		{
			ShowToast("No se pudo borrar el .zar: " + e.Message);
		}
	}

	// Called when a game closes: after trying a fresh .zar, ask whether it worked.
	private void AfterArchiveTest(Game closed)
	{
		if (_zarTest is not { } test || !string.Equals(closed.Archive, test.Archive, StringComparison.OrdinalIgnoreCase)) return;
		_zarTest = null;
		ShowChoice("¿Funcionó bien el juego comprimido?", "Si se jugó igual que siempre, puedes borrar la carpeta original y quedarte solo con el .zar.", new()
		{
			new("Sí: borrar la carpeta original", "La carpeta va a la papelera; te quedas con el .zar.", () => "", _ => RecycleOriginal(test.Folder)),
			new("No: borrar el .zar y quedarme con la carpeta", "El .zar va a la papelera; el juego queda como antes.", () => "", _ => RecycleArchive(test.Archive)),
			new("Decidir más tarde", "Se quedan las dos versiones.", () => "", _ => { }),
		});
	}

	// ---- Choice -------------------------------------------------------------------------------

	// A question with a few answers over everything; A picks, B takes the last answer.
	private Border BuildChoice()
	{
		var content = new StackPanel { Spacing = 4 };
		content.Children.Add(_choiceTitle);
		content.Children.Add(_choiceMessage);
		content.Children.Add(_choiceList);
		_choiceTitle.TextWrapping = TextWrapping.Wrap;
		_choiceMessage.Foreground = Palette.Brush(Palette.Muted);
		var sheet = Sheet("", "▲ ▼ elegir     A aceptar", content, () => PickChoice(_choices.Count - 1), "B  Cerrar");
		sheet.Width = 1200;
		sheet.HorizontalAlignment = HorizontalAlignment.Center;
		sheet.VerticalAlignment   = VerticalAlignment.Center;
		sheet.PointerPressed += (_, e) => e.Handled = true;
		return new Border { Width = 1920, Height = 1080, IsVisible = false, Background = Palette.Brush(Palette.Night, 0.82), Child = sheet };
	}

	private void ShowChoice(string title, string message, List<Setting> choices)
	{
		if (_zone != Zone.Choice) _beforeChoice = _zone is Zone.GameOptions or Zone.TextEntry or Zone.Keys ? Zone.Tiles : _zone;
		_gameSheet.IsVisible = false;
		_choiceTitle.Text   = title;
		_choiceMessage.Text = message;
		_choices     = choices;
		_choiceIndex = 0;
		BuildChoiceRows();
		_choiceSheet.IsVisible = true;
		_zone = Zone.Choice;
		WindowState = WindowState.FullScreen;
		Activate();
	}

	private void BuildChoiceRows()
	{
		_choiceList.Children.Clear();
		for (var i = 0; i < _choices.Count; i++)
		{
			var index = i;
			_choiceList.Children.Add(OptionRow(_choices[i], i == _choiceIndex, _ => PickChoice(index)));
		}
	}

	private void PickChoice(int index)
	{
		if (_zone != Zone.Choice || index < 0 || index >= _choices.Count) return;
		var choice = _choices[index];
		_choiceSheet.IsVisible = false;
		_zone = _beforeChoice;
		choice.Change(1);
	}

	private void ChoiceInput(PadInput input)
	{
		switch (input)
		{
			case PadInput.Up:   _choiceIndex = Math.Max(0, _choiceIndex - 1); break;
			case PadInput.Down: _choiceIndex = Math.Min(_choices.Count - 1, _choiceIndex + 1); break;
			case PadInput.Accept: PickChoice(_choiceIndex); return;
			case PadInput.Back: PickChoice(_choices.Count - 1); return;
		}
		BuildChoiceRows();
	}

	// ---- Context menu -------------------------------------------------------------------------

	// Right click on a game: its actions, as in SharpEmu's and KytyPS5's launchers.
	private void ShowGameMenu(Control tile, Game game)
	{
		var items = new List<MenuItem>();
		MenuItem Item(string header, Action click, bool enabled = true)
		{
			var item = new MenuItem { Header = header, IsEnabled = enabled, FontSize = 16 };
			item.Click += (_, _) => click();
			items.Add(item);
			return item;
		}
		Item("Jugar", () => Launch(game));
		Item("Opciones del juego", () => ShowGameOptions(game));
		if (game.Archive is null)
		{
			if (_packing == game) Item($"Comprimiendo… {_packProgress * 100:0} %", () => { }, enabled: false);
			else if (GameArchive.Packed(game) is { } packed) Item("Borrar la carpeta original (ya está en .zar)", () => ShowChoice("¿Borrar la carpeta original?",
			    $"{game.Name} ya está comprimido en {System.IO.Path.GetFileName(packed)}. La carpeta va a la papelera.", new()
			    {
				    new("Sí, borrar la carpeta", "", () => "", _ => RecycleOriginal(game.Dir)),
				    new("No", "", () => "", _ => { }),
			    }));
			else Item("Comprimir en .zar", () => PackGame(game), enabled: _packing is null);
		}
		else
		{
			Item("Borrar este .zar", () => ShowChoice("¿Borrar el juego comprimido?", $"{System.IO.Path.GetFileName(game.Archive)} va a la papelera.", new()
			{
				new("Sí, borrar el .zar", "", () => "", _ => RecycleArchive(game.Archive)),
				new("No", "", () => "", _ => { }),
			}));
		}
		if (ShaderPrep.Available)
		{
			if (_prepping == game) Item(_prepReady ? $"Preparando shaders… {_prepProgress * 100:0.0} % (ya se puede jugar)" : $"Preparando shaders… {_prepProgress * 100:0.0} %", AskCancelPrep);
			else Item("Preparar shaders", () => AskPrep(game), enabled: _prepping is null);
		}
		Item("Abrir la carpeta", () => Process.Start(new ProcessStartInfo(game.Archive is null ? game.Dir : System.IO.Path.GetDirectoryName(game.Archive)!) { UseShellExecute = true }));
		Item("Copiar el ID del título", () => { Clipboard?.SetTextAsync(game.TitleId); ShowToast($"{game.TitleId} copiado"); });
		var menu = new ContextMenu { ItemsSource = items, FontFamily = Palette.Font };
		menu.Open(tile);
	}
	private async void DownloadPatches(Game game)
	{
		if (_downloadingPatches) return;
		_downloadingPatches = true;
		BuildGameRows();
		try
		{
			var (found, added) = await GamePatches.DownloadAsync(_emulator.BaseDir, game, CancellationToken.None);
			if (_optionsGame == game) _patches = GamePatches.Load(_emulator.BaseDir, game);
			// Nothing new is said in a box, not a toast: the list does not change.
			if (added == 0 && _zone == Zone.GameOptions && _optionsGame == game)
			{
				_downloadingPatches = false;
				if (found == 0)
					PatchesBox(game, "No se encontraron parches para este juego",
					    $"La colección de parches de la comunidad no tiene ninguno para {game.Name} (versión {game.Version}). Puedes volver a buscar más adelante.");
				else
					PatchesBox(game, "Ya se añadieron todos los parches disponibles",
					    "Este juego ya tiene todos los parches de la comunidad para su versión. Actívalos o desactívalos en la lista.");
				return;
			}
			ShowToast(added > 0 ? $"{added} parche(s) nuevo(s): actívalos en la lista"
			    : found == 0 ? "No se encontraron parches para este juego" : "Ya se añadieron todos los parches disponibles");
		}
		catch (Exception e) when (e is HttpRequestException or TaskCanceledException or IOException or System.Text.Json.JsonException)
		{
			ShowToast("No se pudieron descargar los parches: " + e.Message);
		}
		finally
		{
			_downloadingPatches = false;
			if (_zone == Zone.GameOptions) BuildGameRows();
		}
	}

	// A message over the game options; Aceptar goes back to the same row.
	private void PatchesBox(Game game, string title, string message)
	{
		var option = _gameOption;
		ShowChoice(title, message, new() { new("Aceptar", "", () => "", _ =>
		{
			ShowGameOptions(game);
			_gameOption = option;
			BuildGameRows();
		}) });
	}

	// ---- Data ---------------------------------------------------------------------------------

	private void Reload()
	{
		_trophyCache.Clear();
		var roots = _emulator.GameDirs().Concat(_store.ExtraDirs).Distinct(StringComparer.OrdinalIgnoreCase).ToList();
		var previous = _games.ElementAtOrDefault(_index)?.Dir;
		_games = Library.Scan(roots);
		if (SnapshotPath is not null) _games.RemoveAll(g => g.Package is not null); // Pictures for the web: installed games only.
		_index = Math.Max(0, _games.FindIndex(g => g.Dir == previous));
		_row.Children.Clear();
		foreach (var game in _games) _row.Children.Add(Tile(game));
		_row.Children.Add(AddTile());
		for (var i = 0; i < _row.Children.Count; i++)
		{
			var index = i;
			_row.Children[i].Cursor = new Cursor(StandardCursorType.Hand);
			_row.Children[i].PointerPressed += (sender, e) =>
			{
				// Right click: that game's options, like SharpEmu's and KytyPS5's context menu.
				if (e.GetCurrentPoint(sender as Control).Properties.IsRightButtonPressed)
				{
					if (_zone is Zone.Tiles or Zone.Buttons && _running is null && index < _games.Count && _games[index].Package is null)
					{
						Select(index, force: true);
						ShowGameMenu((Control)sender!, _games[index]);
					}
					return;
				}
				TileClicked(index);
			};
		}
		if (!_emulator.Exists) ShowToast("No se encontró Nova5Emulator junto al lanzador");
		Select(_index, force: true);
		WatchFolders(roots);
		InstallPendingPackages();
	}

	// ---- Packages -----------------------------------------------------------------------------

	private readonly Dictionary<string, string> _packageStatus = new(StringComparer.OrdinalIgnoreCase);
	private readonly Dictionary<string, Packages.InstallProgress> _packageProgress = new(StringComparer.OrdinalIgnoreCase);
	private readonly HashSet<string>            _packageFailed = new(StringComparer.OrdinalIgnoreCase);
	private readonly List<FileSystemWatcher>    _watchers = new();
	private bool                                _installingPackages;

	// Installs every new .pkg in the game folders, and unpacks every .rar/.zip/.7z holding a game, one at a time, as soon
	// as it has finished copying. A .pkg that comes out of an archive is installed next.
	private async void InstallPendingPackages()
	{
		if (_installingPackages) return;
		_installingPackages = true;
		try
		{
			while (_games.FirstOrDefault(g => g.Package is { } p && !_packageFailed.Contains(p)) is { Package: { } package } game)
			{
				SetPackageProgress(package, new Packages.InstallProgress("Esperando a que termine de copiarse…", null));
				if (!await Packages.WaitUntilStableAsync(package, CancellationToken.None))
				{
					_packageProgress.Remove(package);
					_packageFailed.Add(package);
					continue;
				}
				var archive = Archives.IsArchive(package);
				ShowToast(archive ? $"Descomprimiendo {game.Name}…" : $"Instalando {game.Name}…");
				try
				{
					var progress = new Progress<Packages.InstallProgress>(p => SetPackageProgress(package, p));
					await Task.Run(() => archive ? Archives.ExtractAsync(package, progress, CancellationToken.None)
					                             : Packages.InstallAsync(package, progress, CancellationToken.None));
					_packageStatus.Remove(package);
					_packageProgress.Remove(package);
					Reload();
					ShowToast(archive && Directory.EnumerateFiles(Archives.OutputFor(package), "*.pkg", SearchOption.AllDirectories).Any()
					    ? $"{game.Name} descomprimido; ahora se instala su PKG" : $"{game.Name} está listo para jugar");
				}
				catch (Exception e) when (e is IOException or InvalidDataException or InvalidOperationException or UnauthorizedAccessException or System.ComponentModel.Win32Exception)
				{
					try
					{
						if (archive) Archives.CleanUp(package);
						else Packages.CleanUp(package);
					}
					catch (IOException) { }
					_packageFailed.Add(package);
					_packageProgress.Remove(package);
					SetPackageStatus(package, (archive ? "No se pudo descomprimir: " : "No se pudo instalar: ") + e.Message);
					ShowToast(archive ? $"No se pudo descomprimir {game.Name}" : $"No se pudo instalar {game.Name}");
				}
			}
		}
		finally
		{
			_installingPackages = false;
		}
	}

	private void SetPackageProgress(string package, Packages.InstallProgress progress)
	{
		_packageProgress[package] = progress;
		SetPackageStatus(package, ShortProgress(progress));
	}

	private void SetPackageStatus(string package, string status)
	{
		_packageStatus[package] = status;
		var index = _games.FindIndex(g => g.Package == package);
		if (index < 0) return;
		if (_row.Children[index] is Border { Child: StackPanel panel })
		{
			if (panel.Children.OfType<TextBlock>().LastOrDefault() is { } label) label.Text = status;
			if (panel.Children.OfType<ProgressBar>().FirstOrDefault() is { } bar) ShowProgress(bar, _packageProgress.GetValueOrDefault(package));
		}
		if (index == _index) Select(_index, force: true);
	}

	// The tile shows the percentage and the time left; the details line under the title shows everything.
	private static readonly System.Globalization.CultureInfo Es = System.Globalization.CultureInfo.GetCultureInfo("es-ES");

	private static string ShortProgress(Packages.InstallProgress p) =>
	    p.Fraction is { } f && p.Total > 0 ? $"{(p.Step == "Descomprimiendo" ? "Descomprimiendo" : "Instalando")} {f * 100:0} %" + (p.Remaining is { } r ? " · " + TimeLeft(r) : "") : p.Step;

	private static string LongProgress(Packages.InstallProgress p)
	{
		if (p.Total == 0) return p.Step;
		var parts = new List<string> { $"{p.Step}: {Gb(p.Done)} de {Gb(p.Total)} GB ({(p.Fraction ?? 0) * 100:0} %)" };
		if (p.Speed > 0) parts.Add((p.Speed / (1 << 20)).ToString("0", Es) + " MB/s");
		if (p.Remaining is { } r) parts.Add(TimeLeft(r));
		if (p.FilesTotal > 0) parts.Add($"{p.FilesDone.ToString("N0", Es)} de {p.FilesTotal.ToString("N0", Es)} archivos");
		return string.Join(" · ", parts);
	}

	// Sizes in GB as Windows shows them (1024³ bytes), so they match the folder in the Explorer.
	private static string Gb(long bytes) => (bytes / (double)(1L << 30)).ToString("0.0", Es);

	private static string TimeLeft(TimeSpan left) =>
	    left.TotalMinutes < 1 ? "menos de 1 min" : left.TotalHours < 1 ? $"quedan {Math.Ceiling(left.TotalMinutes):0} min" : $"quedan {(int)left.TotalHours} h {left.Minutes} min";

	private static ProgressBar ProgressLine(double width) => new()
	{
		Minimum = 0, Maximum = 1, Height = 6, MinHeight = 6, MinWidth = 0, Width = width, CornerRadius = new CornerRadius(3),
		Foreground = Palette.Aurora(), Background = Palette.Brush(Palette.Text, 0.15), IsVisible = false,
	};

	private static void ShowProgress(ProgressBar bar, Packages.InstallProgress? progress)
	{
		bar.IsVisible = progress is not null;
		if (progress is null) return;
		bar.IsIndeterminate = progress.Fraction is null;
		if (progress.Fraction is { } fraction) bar.Value = fraction;
	}

	private void WatchFolders(IEnumerable<string> roots)
	{
		if (_watchers.Count > 0) return;
		foreach (var root in roots.Where(Directory.Exists))
		{
			var watcher = new FileSystemWatcher(root) { IncludeSubdirectories = true };
			foreach (var pattern in Archives.Patterns.Prepend("*.pkg")) watcher.Filters.Add(pattern);
			watcher.EnableRaisingEvents = true;
			watcher.Created += (_, _) => Dispatcher.UIThread.Post(() => { if (_running is null) Reload(); });
			watcher.Renamed += (_, _) => Dispatcher.UIThread.Post(() => { if (_running is null) Reload(); });
			_watchers.Add(watcher);
		}
	}

	private Game? Current => _index < _games.Count ? _games[_index] : null;

	private void Select(int index, bool force = false)
	{
		var count = _games.Count + 1; // The last tile adds a folder.
		index = Math.Clamp(index, 0, count - 1);
		if (index == _index && !force) return;
		_index = index;
		for (var i = 0; i < _row.Children.Count; i++) Focus((Border)_row.Children[i], i == _index && _zone == Zone.Tiles);
		_rowShift.X = -Math.Max(0, _index - (VisibleTiles - 2)) * (TileSize + TileGap);

		var game = Current;
		_title.Text = game?.Name ?? "Añadir juegos";
		_meta.Children.Clear();
		_stats.Children.Clear();
		foreach (var button in _buttons) button.IsVisible = game is not null;
		if (_statsBox is not null) _statsBox.IsVisible = game is not null;
		if (game is null)
		{
			_gameTrophies.IsVisible = false;
			_meta.Children.Add(Text("Elige la carpeta donde tienes tus juegos de PS5 extraídos", 22, FontWeight.Normal, Palette.Muted));
			SetBackground(null);
			_music.Stop();
			return;
		}
		if (game.Package is { } package)
		{
			foreach (var button in _buttons) button.IsVisible = false;
			if (_statsBox is not null) _statsBox.IsVisible = false;
			_gameTrophies.IsVisible = false;
			_meta.Children.Add(Badge(Archives.IsArchive(package) ? "Archivo " + Archives.Label(package) : "Paquete PKG"));
			_meta.Children.Add(Text($"{new FileInfo(package).Length / 1e9:0.0} GB", 22, FontWeight.Normal, Palette.Muted));
			if (_packageProgress.GetValueOrDefault(package) is { } progress)
			{
				var bar = ProgressLine(640);
				ShowProgress(bar, progress);
				_meta.Children.Add(new StackPanel
				{
					Spacing = 10, VerticalAlignment = VerticalAlignment.Center,
					Children = { Text(LongProgress(progress), 22, FontWeight.Normal, Palette.Muted), bar },
				});
			}
			else _meta.Children.Add(Text(_packageStatus.GetValueOrDefault(package, Archives.IsArchive(package) ? "Pendiente de descomprimir" : "Pendiente de instalar"), 22, FontWeight.Normal, Palette.Muted));
			if (_packageFailed.Contains(package)) _meta.Children.Add(Text("· pulsa A para reintentar", 22, FontWeight.Normal, Palette.AuroraA));
			SetBackground(null);
			_music.Stop();
			return;
		}
		ShowGameTrophies(game);
		var patches = GamePatches.Load(_emulator.BaseDir, game);
		_meta.Children.Add(Badge(patches is { HasModes: true } ? $"Modo {ModeLabel(patches.Mode)}"
		    : patches?.Mods.Any(m => m.Enabled) == true ? "Parches activos" : "Instalado"));
		if (game.Archive is not null) _meta.Children.Add(Badge("Comprimido .zar"));
		foreach (var part in new[] { game.TitleId, "v" + game.Version, LastPlayedText(game) })
		{
			if (part.Length == 0) continue;
			_meta.Children.Add(Text(part, 22, FontWeight.Normal, Palette.Muted));
		}
		_stats.Children.Add(new TextBlock { Text = "EN ESTE PC", FontSize = 18, Foreground = Palette.Brush(Palette.Muted), LetterSpacing = 1.5, Margin = new Thickness(0, 0, 0, 14) });
		Stat("Tiempo jugado", PlayTimeText(game));
		Stat("Caché de shaders", _emulator.ShaderStoreFor(game.TitleId) is { } store ? $"Lista · {new FileInfo(store).Length / 1048576.0:0} MB" : "Se crea al jugar");
		Stat("Parche del juego", _emulator.PatchFor(game.TitleId) is null ? "Ninguno" : "Activo");
		Stat("Música del menú", game.Music is null ? "No incluida" : _store.Music ? "Sonando" : "Desactivada");
		SetBackground(game.Background);
		_music.Play(_store.Music ? game.Music : null);
	}

	private void Stat(string label, string value)
	{
		var row = new DockPanel { Margin = new Thickness(0, 9) };
		var v   = Text(value, 22, FontWeight.SemiBold);
		DockPanel.SetDock(v, Dock.Right);
		row.Children.Add(v);
		row.Children.Add(Text(label, 22, FontWeight.Normal));
		_stats.Children.Add(row);
	}

	private string PlayTimeText(Game game)
	{
		var seconds = _store.PlaySeconds.GetValueOrDefault(game.TitleId);
		if (seconds < 60) return "Sin jugar";
		var time = TimeSpan.FromSeconds(seconds);
		return time.TotalHours >= 1 ? $"{(int)time.TotalHours} h {time.Minutes} min" : $"{time.Minutes} min";
	}

	private string LastPlayedText(Game game)
	{
		if (!_store.LastPlayed.TryGetValue(game.TitleId, out var when)) return "";
		var days = (DateTime.Now.Date - when.ToLocalTime().Date).Days;
		return days switch { 0 => "Última vez: hoy", 1 => "Última vez: ayer", _ => $"Última vez: hace {days} días" };
	}

	private async void SetBackground(string? path)
	{
		if (path == _backPath) return;
		_backPath = path;
		var bitmap = path is null ? null : await LoadImage(path, 1920);
		if (path != _backPath) return;
		var (show, hide) = _showingA ? (_backB, _backA) : (_backA, _backB);
		show.Source  = bitmap;
		show.Opacity = bitmap is null ? 0 : 1;
		hide.Opacity = 0;
		_showingA    = !_showingA;
	}

	private async Task<Bitmap?> LoadImage(string path, int width)
	{
		var key = $"{width}|{path}";
		if (_images.TryGetValue(key, out var cached)) return cached;
		var bitmap = await Task.Run(() =>
		{
			try
			{
				using var stream = File.OpenRead(path);
				return Bitmap.DecodeToWidth(stream, width, BitmapInterpolationMode.HighQuality);
			}
			catch (Exception e) when (e is IOException or ArgumentException or NotSupportedException or InvalidOperationException)
			{
				return null;
			}
		});
		if (_images.Count > 48) _images.Clear();
		_images[key] = bitmap;
		return bitmap;
	}

	// ---- Input --------------------------------------------------------------------------------

	private void OnKey(object? sender, KeyEventArgs e)
	{
		PadInput? input = e.Key switch
		{
			Key.Left => PadInput.Left, Key.Right => PadInput.Right, Key.Up => PadInput.Up, Key.Down => PadInput.Down,
			Key.Enter or Key.Space => PadInput.Accept, Key.Back => PadInput.Back,
			Key.Q => PadInput.PrevTab, Key.E or Key.Tab => PadInput.NextTab, Key.Y => PadInput.Options,
			_ => null,
		};
		if (_zone == Zone.Keys && _capturing is { } control)
		{
			e.Handled = true;
			if (e.Key == Key.Escape) StopCapture(null);
			else if (e.Key == Key.Delete) StopCapture("");
			else if (Keymap.Name(e.Key) is { } name) StopCapture(name);
			return;
		}
		if (_zone == Zone.TextEntry)
		{
			if (e.Key is Key.Enter or Key.Escape)
			{
				CloseText(e.Key == Key.Enter);
				e.Handled = true;
			}
			return;
		}
		if (e.Key == Key.F11)
		{
			WindowState = WindowState == WindowState.FullScreen ? WindowState.Maximized : WindowState.FullScreen;
			e.Handled   = true;
			return;
		}
		if (e.Key == Key.Escape)
		{
			ShowChrome(_chrome.Opacity == 0, pin: true);
			e.Handled = true;
			return;
		}
		if (input is null || _running is not null) return;
		e.Handled = true;
		OnInput(input.Value);
	}

	private void OnInput(PadInput input)
	{
		if (_zone == Zone.TextEntry)
		{
			if (input is PadInput.Accept or PadInput.Back) CloseText(input == PadInput.Accept);
			return;
		}
		if (_zone == Zone.Keys)
		{
			KeysInput(input);
			return;
		}
		if (_zone == Zone.Choice)
		{
			ChoiceInput(input);
			return;
		}
		if (_zone == Zone.Notes)
		{
			NotesInput(input);
			return;
		}
		if (_zone == Zone.GameOptions)
		{
			GameOptionsInput(input);
			return;
		}
		if (_zone == Zone.TrophyList)
		{
			TrophyListInput(input);
			return;
		}
		if (_zone == Zone.Prompt)
		{
			if (input == PadInput.Accept) Close();
			if (input == PadInput.Back) ShowPrompt(false);
			return;
		}
		if (_zone == Zone.NovaS && input is not (PadInput.PrevTab or PadInput.NextTab))
		{
			_novas.Input(input);
			return;
		}
		if (input is PadInput.PrevTab or PadInput.NextTab)
		{
			ShowTab((_tab + (input == PadInput.NextTab ? 1 : _tabs.Length - 1)) % _tabs.Length);
			return;
		}
		if (_zone == Zone.Settings)
		{
			SettingsInput(input);
			return;
		}
		if (_zone == Zone.Trophies)
		{
			TrophiesInput(input);
			return;
		}
		switch (input)
		{
			case PadInput.Left when _zone == Zone.Tiles:  Select(_index - 1); break;
			case PadInput.Right when _zone == Zone.Tiles: Select(_index + 1); break;
			case PadInput.Left:  FocusButton(_button - 1); break;
			case PadInput.Right: FocusButton(_button + 1); break;
			case PadInput.Down when _zone == Zone.Tiles && Current is not null: SetZone(Zone.Buttons); break;
			case PadInput.Up when _zone == Zone.Buttons: SetZone(Zone.Tiles); break;
			case PadInput.Options or PadInput.Menu: ShowTab(SettingsTab); break;
			case PadInput.Back when _zone == Zone.Buttons: SetZone(Zone.Tiles); break;
			case PadInput.Back: ShowPrompt(true); break;
			case PadInput.Accept: Activate(_zone == Zone.Tiles ? 0 : _button); break;
		}
	}

	private void Activate(int action)
	{
		var game = Current;
		if (game is null)
		{
			AddFolder();
			return;
		}
		switch (action)
		{
			case 0 when game.Package is { } package:
				if (_packageFailed.Remove(package)) InstallPendingPackages(); // Retry after a failure.
				break;
			case 0: Launch(game); break;
			case 1: ShowGameOptions(game); break;
			case 2: Process.Start(new ProcessStartInfo(game.Dir) { UseShellExecute = true }); break;
		}
	}

	private void SetZone(Zone zone)
	{
		_zone = zone;
		if (zone == Zone.Buttons) _button = 0;
		SetZoneVisuals();
	}

	private void SetZoneVisuals()
	{
		for (var i = 0; i < _row.Children.Count; i++) Focus((Border)_row.Children[i], i == _index && _zone == Zone.Tiles);
		for (var i = 0; i < _buttons.Length; i++) FocusButtonVisual(_buttons[i], _zone == Zone.Buttons && i == _button);
	}

	// A click selects a game; a click on the selected game plays it, like pressing A.
	private void TileClicked(int index)
	{
		if (_zone is not (Zone.Tiles or Zone.Buttons) || _running is not null) return;
		if (index == _index && _zone == Zone.Tiles)
		{
			Activate(0);
			return;
		}
		SetZone(Zone.Tiles);
		Select(index, force: true);
	}

	private void FocusButton(int index)
	{
		_button = Math.Clamp(index, 0, _buttons.Length - 1);
		for (var i = 0; i < _buttons.Length; i++) FocusButtonVisual(_buttons[i], i == _button);
	}

	private void ShowPrompt(bool show)
	{
		if (show) _beforePrompt = _zone;
		_prompt.IsVisible = show;
		_zone = show ? Zone.Prompt : _beforePrompt;
	}

	private void ShowTab(int tab)
	{
		if (Edition.NovaSOnly) tab = NovaSTab; // NovaS on its own has no PS5 side.
		_tab = tab;
		for (var i = 0; i < _tabs.Length; i++)
		{
			_tabs[i].FontWeight = i == tab ? FontWeight.SemiBold : FontWeight.Normal;
			_tabs[i].Foreground = Palette.Brush(i == tab ? Palette.Text : Palette.Muted);
		}
		_gamesPage.IsVisible    = tab == 0;
		_settingsPage.IsVisible = tab == SettingsTab;
		_trophiesPage.IsVisible = tab == TrophiesTab;
		_novas.IsVisible        = tab == NovaSTab;
		if (tab == NovaSTab)
		{
			_zone = Zone.NovaS;
			_music.Stop();
			_novas.Load(SwitchGames.Scan(SwitchRoots()));
		}
		else if (tab == SettingsTab)
		{
			_zone = Zone.Settings;
			_settingsOnCategories = true;
			BuildSettings();
		}
		else if (tab == TrophiesTab)
		{
			_zone = Zone.Trophies;
			BuildTrophies();
		}
		else
		{
			SetZone(Zone.Tiles);
		}
	}

	// ---- Settings -----------------------------------------------------------------------------

	private sealed record Setting(string Label, string Detail, Func<string> Value, Action<int> Change)
	{
		// A section title: drawn as a heading, skipped by the pad.
		public bool Header { get; init; }
	}

	private static Setting Section(string title) => new(title, "", () => "", _ => { }) { Header = true };

	private static readonly string[] Resolutions = { "R1280X720", "R1920X1080", "R2560X1440", "R3840X2160" };
	private static readonly int[] RefreshRates = { 60, 120, 144 };

	private int RefreshRate() =>
	    int.TryParse(_emulator.Ini.Get("GlobalConfiguration", "vblank_frequency"), out var hz) ? hz : 60;

	private List<Setting> Settings() => new()
	{
		Section("Nova5"),
		new("Información de la versión", $"Tienes Nova5 {Updates.Current}. Abre las novedades de esta versión y de las anteriores.",
		    () => "Ver ›", _ => ShowNotes(ReleaseNotes.All.Where(n => n.Version <= Updates.Current).ToList(), firstTime: false)),
		new(_update is null ? "Buscar actualizaciones" : $"Actualizar a Nova5 {_update.Version}",
		    _update is null ? "Consulta en GitHub si hay una versión nueva de Nova5." : "Descarga e instala la versión nueva; tus partidas y ajustes se conservan.",
		    () => _updating ? "Actualizando…" : _update is null ? "Buscar ›" : "Actualizar ›", _ => { if (_update is null) CheckUpdates(true); else StartUpdate(); }),
		new("Actualizaciones", "Avisar: te pregunta antes de instalar. Automáticas: se instalan al abrir Nova5. Desactivadas: no se conecta para esto.",
		    () => _store.UpdateMode switch { "auto" => "Automáticas", "no" => "Desactivadas", _ => "Avisar" }, step =>
		{
			string[] modes = { "avisar", "auto", "no" };
			_store.UpdateMode = modes[(Math.Max(Array.IndexOf(modes, _store.UpdateMode), 0) + step + modes.Length) % modes.Length];
			_store.Save();
		}),
		Section("Gráficos"),
		Toggle("Pantalla completa al jugar", "Los juegos se abren a pantalla completa; si no, en una ventana.", "fullscreen_enabled", true),
		new("Resolución", "Resolución a la que se dibujan los juegos. Elige la de tu monitor para que se vean nítidos; más alta exige más a la gráfica.",
		    () => _emulator.Ini.Get("GlobalConfiguration", "screen_resolution")?.TrimStart('R').Replace('X', '×') ?? "1920×1080",
		    step =>
		    {
			    var current = Array.IndexOf(Resolutions, _emulator.Ini.Get("GlobalConfiguration", "screen_resolution") ?? "R1920X1080");
			    SetIni("screen_resolution", Resolutions[(Math.Max(current, 0) + step + Resolutions.Length) % Resolutions.Length]);
		    }),
		// Emulated display refresh. At 119 Hz or more games are told the screen is 120 Hz, so
		// those with a 120 Hz mode can pass 60 FPS; games that count vblanks could run faster.
		new("Frecuencia de pantalla", "Los juegos con modo de 120 Hz pueden pasar de 60 FPS con 120 o 144 Hz. Si un juego va acelerado, vuelve a 60 Hz.",
		    () => $"{RefreshRate()} Hz", step =>
		{
			var current = Array.IndexOf(RefreshRates, RefreshRate());
			SetIni("vblank_frequency", RefreshRates[(Math.Max(current, 0) + step + RefreshRates.Length) % RefreshRates.Length].ToString());
		}),
		Cycle("Sincronización (VSync)", "Mailbox: fluido y sin cortes, recomendado. VSync: sin cortes pero con algo más de retraso. Sin límite: el menor retraso, puede verse cortada la imagen.",
		    "present_mode", "Mailbox", ("Mailbox", "Mailbox"), ("Fifo", "VSync"), ("Immediate", "Sin límite")),
		Toggle("HDR", "Imagen HDR si tu monitor y el juego lo admiten.", "hdr_enabled", false),
		Toggle("Contador de FPS en pantalla", "Muestra los FPS y el uso de la gráfica y el procesador mientras juegas.", "perf_overlay_enabled", false),
		Toggle("Ocultar el cursor", "Oculta el puntero del ratón dentro del juego.", "hide_cursor_enabled", false),
		Section("Sistema"),
		new("Nombre de usuario", "El nombre que ven los juegos, por ejemplo al guardar o en los marcadores. Se escribe con el teclado.",
		    () => _emulator.Ini.Get("GlobalConfiguration", "user_name") is { Length: > 0 } name ? name : "Nova5",
		    _ => AskText("Nombre de usuario", _emulator.Ini.Get("GlobalConfiguration", "user_name") ?? "Nova5", value => SetIni("user_name", value))),
		Cycle("Idioma de la consola", "El idioma que los juegos usan para textos y voces, si lo tienen.", "console_language", "1", ConsoleLanguages),
		Toggle("Saltar avisos de inicio", "Pide a los juegos que se salten las pantallas de aviso del principio, si lo permiten.", "skip_notice_screen", false),
		Section("Rendimiento"),
		Toggle("Caché de shaders", "Guarda los shaders ya preparados para que los juegos carguen más rápido y tengan menos tirones.", "shader_cache_enabled", true),
		Toggle("Compatibilidad de CPU", "Solo si un juego se cierra al arrancar en tu procesador. Puede bajar el rendimiento.", "amd_cpu_enabled", false),
		Section("Mando"),
		new("Teclado y ratón", "Qué tecla o botón del ratón hace cada botón del DualSense. Por defecto L2 y R2 van en Z y C.", () => "Configurar ›", _ => ShowKeys()),
		Percent("Vibración del mando", "Intensidad de la vibración (0 % la apaga).", "controller_vibration_intensity", 100),
		Percent("Altavoz del mando", "Volumen del altavoz del DualSense.", "controller_speaker_volume", 50),
		Cycle("Color de la barra de luz", "Color de la luz del DualSense. Automático: el que pida el juego.", "controller_color", "",
		    ("", "Automático"), ("#0050FF", "Azul"), ("#5EEAD4", "Aurora"), ("#8B5CF6", "Morado"), ("#FF2D55", "Rojo"), ("#30D158", "Verde"), ("#FFFFFF", "Blanco")),
		Section("Trofeos"),
		Toggle("Avisos de trofeos", "Muestra un aviso con sonido al conseguir un trofeo mientras juegas.", "trophy_enabled", true),
		Cycle("Duración del aviso", "Cuánto tiempo se queda el aviso de trofeo en pantalla.", "trophy_duration_seconds", "6",
		    ("5", "5 s"), ("6", "6 s"), ("8", "8 s"), ("10", "10 s")),
		Section("Audio"),
		new("Música del menú", "Suena la música de menú de cada juego al seleccionarlo.", () => _store.Music ? "Sí" : "No", _ =>
		{
			_store.Music   = !_store.Music;
			_music.Enabled = _store.Music;
			_store.Save();
		}),
		new("Volumen de la música", "Volumen de la música del menú.", () => $"{_store.MusicVolume * 100:0} %", step =>
		{
			_store.MusicVolume = Math.Clamp(Math.Round(_store.MusicVolume + step * 0.1, 1), 0, 1);
			_music.Volume      = _store.MusicVolume;
			_store.Save();
		}),
		Section("Biblioteca"),
		new("Añadir carpeta de juegos", "Elige otra carpeta con juegos de PS5, archivos .pkg o juegos en .rar, .zip o .7z para añadirlos a la biblioteca.", () => "Elegir ›", _ => AddFolder()),
		new("Donar", "Nova5 es gratis. Si te resulta útil, puedes apoyarlo en ko-fi.com/nova5emu.", () => "Ko-fi ›",
		    _ => Process.Start(new ProcessStartInfo("https://ko-fi.com/nova5emu") { UseShellExecute = true })),
		new("Salir de Nova5", "Cierra el lanzador.", () => "Salir ›", _ => ShowPrompt(true)),
	};

	private Setting DemonsSoulsToggle(string label, string detail, string key)
	{
		var variable = DemonsSoulsOptions.Switches.Single(s => s.Key == key).Variable;
		bool Enabled() => DemonsSoulsOptions.Effective(_emulator.Ini.Get("GlobalConfiguration", key), Environment.GetEnvironmentVariable(variable));
		return new(label, detail, () => Enabled() ? "Sí" : "No", _ => SetIni(key, Enabled() ? "false" : "true"));
	}

	private static readonly (string, string)[] ConsoleLanguages =
	{
		("20", "Español (Latinoamérica)"), ("3", "Español (España)"), ("1", "Inglés (EE. UU.)"), ("18", "Inglés (Reino Unido)"),
		("17", "Portugués (Brasil)"), ("7", "Portugués (Portugal)"), ("2", "Francés"), ("4", "Alemán"), ("5", "Italiano"),
		("0", "Japonés"), ("9", "Coreano"), ("11", "Chino simplificado"), ("10", "Chino tradicional"), ("8", "Ruso"), ("16", "Polaco"),
	};

	// A Nova5.ini value chosen from a list: left and right move through it.
	private Setting Cycle(string label, string detail, string key, string fallback, params (string Value, string Label)[] options)
	{
		int Current() => Math.Max(0, Array.FindIndex(options, o => o.Value == (_emulator.Ini.Get("GlobalConfiguration", key) ?? fallback)));
		return new(label, detail, () => options[Current()].Label,
		    step => SetIni(key, options[(Current() + step + options.Length) % options.Length].Value));
	}

	// A setting of one game: "General (…)" follows the general one; any other choice is kept in [<title id>] only for it.
	private Setting GameCycle(string titleId, string label, string detail, string key, string fallback, params (string Value, string Label)[] options)
	{
		string? Own() => _emulator.Ini.Get(titleId, key) is { Length: > 0 } own ? own : null;
		string General() => options.FirstOrDefault(o => o.Value == (_emulator.Ini.Get("GlobalConfiguration", key) ?? fallback)).Label ?? options[0].Label;
		int Current() => Own() is { } own ? Array.FindIndex(options, o => o.Value == own) + 1 : 0; // 0 is General.
		return new(label, detail, () => Current() > 0 ? options[Current() - 1].Label : $"General ({General()})", step =>
		{
			var next = (Math.Max(Current(), 0) + step + options.Length + 1) % (options.Length + 1);
			if (next == 0) _emulator.Ini.Remove(titleId, key);
			else _emulator.Ini.Set(titleId, key, options[next - 1].Value);
			_emulator.Ini.Save();
		});
	}

	private Setting GameToggle(string titleId, string label, string detail, string key, bool fallback) =>
	    GameCycle(titleId, label, detail, key, fallback ? "true" : "false", ("true", "Sí"), ("false", "No"));

	private Setting Percent(string label, string detail, string key, int fallback)
	{
		int Current() => int.TryParse(_emulator.Ini.Get("GlobalConfiguration", key), out var value) ? value : fallback;
		return new(label, detail, () => $"{Current()} %", step => SetIni(key, Math.Clamp(Current() + step * 10, 0, 100).ToString()));
	}

	private Setting Toggle(string label, string detail, string key, bool fallback) =>
	    new(label, detail, () => _emulator.Ini.GetBool("GlobalConfiguration", key, fallback) ? "Sí" : "No",
	        _ => SetIni(key, _emulator.Ini.GetBool("GlobalConfiguration", key, fallback) ? "false" : "true"));

	private void SetIni(string key, string value)
	{
		_emulator.Ini.Set("GlobalConfiguration", key, value);
		_emulator.Ini.Save();
	}

	// Each option: its name and what it does on the left, its value in a pill on the right (filled when on).
	// The section titles of Settings(), each a category; the options of one run up to the next title.
	private static List<int> Categories(List<Setting> settings) =>
	    Enumerable.Range(0, settings.Count).Where(i => settings[i].Header).ToList();

	private static (int Header, int End) CategoryOf(List<Setting> settings, int index)
	{
		var headers = Categories(settings);
		var header  = headers.LastOrDefault(h => h <= index);
		var end     = headers.FirstOrDefault(h => h > header, settings.Count);
		return (header, end);
	}

	private void BuildSettings(bool reveal = false)
	{
		var settings = Settings();
		_setting = Math.Clamp(_setting, 0, settings.Count - 1);
		while (settings[_setting].Header && _setting < settings.Count - 1) _setting++;
		var (category, end) = CategoryOf(settings, _setting);

		_settingsCategories.Children.Clear();
		foreach (var header in Categories(settings))
		{
			var first = header + 1;
			var chosen = header == category;
			var label = new TextBlock
			{
				Text = settings[header].Label, FontSize = 23, FontWeight = chosen ? FontWeight.Bold : FontWeight.SemiBold,
				Foreground = Palette.Brush(chosen ? Palette.Text : Palette.Muted),
			};
			var item = new Border
			{
				Child = label, Padding = new Thickness(22, 14), CornerRadius = new CornerRadius(16),
				Background = chosen ? Palette.Brush(Colors.White, _settingsOnCategories ? 0.10 : 0.06) : Brushes.Transparent,
				BorderBrush = chosen && _settingsOnCategories ? Palette.Aurora() : Brushes.Transparent, BorderThickness = new Thickness(2),
				Cursor = new Cursor(StandardCursorType.Hand),
			};
			item.PointerPressed += (_, _) =>
			{
				if (_zone != Zone.Settings) return;
				_setting = first;
				_settingsOnCategories = true;
				_settingsScroll.Offset = default;
				BuildSettings();
			};
			_settingsCategories.Children.Add(item);
		}

		_settingsList.Children.Clear();
		_settingsList.Children.Add(SectionTitle(settings[category].Label));
		Control? focusedRow = null;
		for (var i = category + 1; i < end; i++)
		{
			var index = i;
			var row = OptionRow(settings[i], i == _setting && !_settingsOnCategories, step =>
			{
				if (_zone != Zone.Settings) return;
				_setting = index;
				_settingsOnCategories = false;
				Settings()[index].Change(step);
				if (_zone == Zone.Settings && _tab == SettingsTab) BuildSettings();
			});
			if (i == _setting) focusedRow = row;
			_settingsList.Children.Add(row);
		}
		// With the pad or the keys, the chosen row always stays in view.
		if (reveal && !_settingsOnCategories && focusedRow is not null)
			Dispatcher.UIThread.Post(() => focusedRow.BringIntoView(), DispatcherPriority.Loaded);
	}
	// One option row: its name and what it does on the left, its value in a pill on the right (filled when on).
	// A left click passes +1 (next value), a right click -1.
	private static Border OptionRow(Setting setting, bool focused, Action<int> click)
	{
		var text = new StackPanel { Spacing = 3, VerticalAlignment = VerticalAlignment.Center };
		text.Children.Add(new TextBlock { Text = setting.Label, FontSize = 25, FontWeight = FontWeight.SemiBold, Foreground = Palette.Brush(Palette.Text), TextWrapping = TextWrapping.Wrap });
		if (setting.Detail.Length > 0)
			text.Children.Add(new TextBlock { Text = setting.Detail, FontSize = 18, Foreground = Palette.Brush(Palette.Muted), TextWrapping = TextWrapping.Wrap });
		var shown = setting.Value();
		var on    = shown == "Sí";
		var value = new Border
		{
			Padding = new Thickness(18, 7), CornerRadius = new CornerRadius(999), MinWidth = 76, VerticalAlignment = VerticalAlignment.Center,
			Margin = new Thickness(28, 0, 0, 0), IsVisible = shown.Length > 0,
			Background = on ? Palette.Aurora() : Palette.Brush(Colors.White, 0.08),
			Child = new TextBlock
			{
				Text = shown, FontSize = 19, FontWeight = FontWeight.Bold, HorizontalAlignment = HorizontalAlignment.Center,
				Foreground = on ? Palette.Brush(Color.Parse("#081022")) : Palette.Brush(Palette.Text, 0.85),
			},
		};
		var row = new DockPanel();
		DockPanel.SetDock(value, Dock.Right);
		row.Children.Add(value);
		row.Children.Add(text);
		var item = new Border
		{
			Child = row, Padding = new Thickness(26, 15), CornerRadius = new CornerRadius(18), Margin = new Thickness(0, 0, 14, 0),
			Background = focused ? Palette.Brush(Colors.White, 0.09) : Brushes.Transparent,
			BorderBrush = focused ? Palette.Aurora() : Brushes.Transparent, BorderThickness = new Thickness(2),
			Cursor = new Cursor(StandardCursorType.Hand),
		};
		item.PointerPressed += (_, e) => click(e.GetCurrentPoint(item).Properties.IsRightButtonPressed ? -1 : 1);
		return item;
	}

	// The next selectable row in that direction (section titles are skipped); stays put at either end.
	private static int Step(List<Setting> list, int from, int direction)
	{
		for (var i = from + direction; i >= 0 && i < list.Count; i += direction)
			if (!list[i].Header) return i;
		return from;
	}

	private static Control SectionTitle(string title) => new TextBlock
	{
		Text = title.ToUpperInvariant(), FontSize = 17, FontWeight = FontWeight.Bold, LetterSpacing = 1.6,
		Foreground = Palette.Brush(Palette.AuroraA), Margin = new Thickness(26, 18, 0, 2),
	};

	// On the categories: up and down pick one, A or right enters its options. On the options: up and down move inside
	// the category, A or right changes the value, left takes the previous one, B goes back to the categories.
	private void SettingsInput(PadInput input)
	{
		var settings = Settings();
		var headers  = Categories(settings);
		var (category, end) = CategoryOf(settings, _setting);
		if (_settingsOnCategories)
		{
			var at = headers.IndexOf(category);
			switch (input)
			{
				case PadInput.Up when at > 0: _setting = headers[at - 1] + 1; _settingsScroll.Offset = default; break;
				case PadInput.Down when at < headers.Count - 1: _setting = headers[at + 1] + 1; _settingsScroll.Offset = default; break;
				case PadInput.Right or PadInput.Accept: _settingsOnCategories = false; break;
				case PadInput.Back: ShowTab(0); return;
			}
		}
		else
		{
			switch (input)
			{
				case PadInput.Up:    if (Step(settings, _setting, -1) is var up && up > category) _setting = up; break;
				case PadInput.Down:  if (Step(settings, _setting, 1) is var down && down < end) _setting = down; break;
				case PadInput.Left:  settings[_setting].Change(-1); break;
				case PadInput.Right or PadInput.Accept: settings[_setting].Change(1); break;
				case PadInput.Back:  _settingsOnCategories = true; break;
			}
		}
		if (_zone == Zone.Settings) BuildSettings(reveal: true);
	}
	private async void AddFolder()
	{
		var folders = await StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions { Title = "Carpeta con tus juegos de PS5", AllowMultiple = false });
		var path    = folders.FirstOrDefault()?.TryGetLocalPath();
		if (path is null) return;
		if (!_store.ExtraDirs.Contains(path, StringComparer.OrdinalIgnoreCase)) _store.ExtraDirs.Add(path);
		_store.Save();
		var before = _games.Count;
		Reload();
		ShowToast(_games.Count > before ? $"{_games.Count - before} juego(s) añadido(s)" : "No se encontraron juegos en esa carpeta");
		if (_tab != 0) ShowTab(0);
	}

	// ---- Updates ------------------------------------------------------------------------------

	private async void CheckUpdates(bool manual)
	{
		if (manual) ShowToast("Buscando actualizaciones…");
		_update = await Updates.CheckAsync();
		if (_tab == SettingsTab) BuildSettings();
		if (_update is null)
		{
			if (manual) ShowToast($"Tienes la última versión ({Updates.Current})");
			return;
		}
		_updateText.Text      = $"Actualizar a {_update.Version}";
		_updatePill.IsVisible = true;
		// NovaS on its own shows the update as one more round button on its home.
		if (Edition.NovaSOnly) _novas.OfferUpdate($"Actualizar a {_update.Version}", StartUpdate);
		if (_store.UpdateMode == "auto" && !manual) StartUpdate();
		else if (Edition.NovaSOnly) _novas.Toast($"NovaS {_update.Version} está disponible: pulsa «Actualizar a {_update.Version}»");
		else ShowToast($"Nova5 {_update.Version} está disponible: pulsa «Actualizar» arriba o en Ajustes");
	}

	private async void StartUpdate()
	{
		if (_update is not { } update || _updating) return;
		if (_running is not null)
		{
			if (Edition.NovaSOnly) _novas.Toast("Cierra el juego para actualizar NovaS");
			else ShowToast("Cierra el juego para actualizar Nova5");
			return;
		}
		_updating = true;
		_music.Stop();
		try
		{
			var progress = new Progress<double>(p =>
			{
				_updateText.Text = $"Descargando {p * 100:0} %";
				if (Edition.NovaSOnly) _novas.Toast($"Descargando NovaS {update.Version}: {p * 100:0} %");
			});
			var setup    = await Updates.DownloadAsync(update, progress);
			_updateText.Text = "Instalando…";
			Updates.Install(setup, _emulator.BaseDir);
			Close(); // The setup replaces the files and opens Nova5 again.
		}
		catch (Exception e) when (e is HttpRequestException or IOException or InvalidDataException or TaskCanceledException or System.ComponentModel.Win32Exception)
		{
			_updating        = false;
			_updateText.Text = $"Actualizar a {update.Version}";
			ShowToast("No se pudo actualizar: " + e.Message);
		}
	}

	// ---- Launch -------------------------------------------------------------------------------

	private void Launch(Game game)
	{
		if (!_emulator.Exists)
		{
			ShowToast("No se encontró Nova5Emulator junto al lanzador");
			return;
		}
		try
		{
			_music.Stop();
			_running      = _emulator.Launch(game);
			_runningGame  = game;
			_started      = DateTime.UtcNow;
			_running.EnableRaisingEvents = true;
			_running.Exited += (_, _) => Dispatcher.UIThread.Post(GameExited);
			WindowState = WindowState.Minimized;
		}
		catch (Exception e) when (e is InvalidOperationException or System.ComponentModel.Win32Exception)
		{
			_running = null;
			ShowToast("No se pudo abrir el emulador: " + e.Message);
		}
	}

	// ---- NovaS (Switch 2) -----------------------------------------------------------------------

	// The PS5 game folders also hold Switch 2 games; NovaS adds its own.
	private List<string> SwitchRoots() =>
	    _emulator.GameDirs().Concat(_store.ExtraDirs).Concat(_store.SwitchDirs).Distinct(StringComparer.OrdinalIgnoreCase).ToList();

	// A cover for a Switch 2 game that has none (or a nicer one): any picture the player has.
	private async void ChangeSwitchCover(SwitchGame game)
	{
		var files = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
		{
			Title = $"Portada para {game.Name}", AllowMultiple = false,
			FileTypeFilter = new[] { new FilePickerFileType("Imágenes") { Patterns = new[] { "*.png", "*.jpg", "*.jpeg", "*.webp", "*.bmp" } } },
		});
		var path = files.FirstOrDefault()?.TryGetLocalPath();
		if (path is null) return;
		try
		{
			SwitchGames.SetCover(game, path);
			ShowToast($"Portada de {game.Name} cambiada");
		}
		catch (Exception e) when (e is IOException or UnauthorizedAccessException)
		{
			ShowToast("No se pudo guardar la portada: " + e.Message);
		}
		_novas.Load(SwitchGames.Scan(SwitchRoots()));
	}

	// The console's prod.keys / title.keys into NovaS.
	private async void InstallSwitchKeys()
	{
		var files = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
		{
			Title = "Claves de tu consola (prod.keys y title.keys)", AllowMultiple = true,
			FileTypeFilter = new[] { new FilePickerFileType("Claves") { Patterns = new[] { "*.keys" } } },
		});
		var paths = files.Select(f => f.TryGetLocalPath()).OfType<string>().ToList();
		if (paths.Count == 0) return;
		try
		{
			var copied = SwitchGames.InstallKeys(paths);
			ShowToast(copied > 0 ? $"Claves instaladas ({copied})" : "Elige archivos .keys (prod.keys, title.keys)");
		}
		catch (Exception e) when (e is IOException or UnauthorizedAccessException)
		{
			ShowToast("No se pudieron copiar las claves: " + e.Message);
		}
		_novas.Load(SwitchGames.Scan(SwitchRoots()));
	}

	// The console's firmware: its .zip, or any .nca of its folder (one picker for both).
	private async void InstallSwitchFirmware()
	{
		var files = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
		{
			Title = "Firmware de tu consola: su .zip o cualquier .nca de su carpeta", AllowMultiple = false,
			FileTypeFilter = new[] { new FilePickerFileType("Firmware") { Patterns = new[] { "*.zip", "*.nca" } } },
		});
		var path = files.FirstOrDefault()?.TryGetLocalPath();
		if (path is null) return;
		var source = path.EndsWith(".zip", StringComparison.OrdinalIgnoreCase) ? path : System.IO.Path.GetDirectoryName(path)!;
		ShowToast("Instalando el firmware…");
		var last = -1;
		var progress = new Progress<double>(p =>
		{
			var percent = (int)(p * 100);
			if (percent / 10 != last / 10) ShowToast($"Instalando el firmware… {percent} %");
			last = percent;
		});
		try
		{
			var count = await Task.Run(() => SwitchGames.InstallFirmware(source, progress));
			ShowToast($"Firmware instalado ({count} archivos)");
		}
		catch (Exception e) when (e is IOException or InvalidDataException or UnauthorizedAccessException)
		{
			ShowToast("No se pudo instalar el firmware: " + e.Message);
		}
		_novas.Load(SwitchGames.Scan(SwitchRoots()));
	}

	// A Switch 2 game full screen in NovaSEmulator.
	private void LaunchSwitch(SwitchGame game)
	{
		if (_running is not null) return;
		if (!SwitchGames.HasEngine)
		{
			ShowToast("Falta el motor de NovaS: actualiza Nova5");
			return;
		}
		try
		{
			_music.Stop();
			_running     = SwitchGames.Start(game);
			_runningGame = null;
			_started     = DateTime.UtcNow;
			_running.EnableRaisingEvents = true;
			_running.Exited += (_, _) => Dispatcher.UIThread.Post(GameExited);
			WindowState = WindowState.Minimized;
		}
		catch (Exception e) when (e is InvalidOperationException or System.ComponentModel.Win32Exception)
		{
			_running = null;
			ShowToast("No se pudo abrir NovaS: " + e.Message);
		}
	}

	private async void AddSwitchFolder()
	{
		var folders = await StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions { Title = "Carpeta con tus juegos de Switch 2 (.nsp, .xci, .nup o descifrados)", AllowMultiple = false });
		var path    = folders.FirstOrDefault()?.TryGetLocalPath();
		if (path is null) return;
		if (!_store.SwitchDirs.Contains(path, StringComparer.OrdinalIgnoreCase)) _store.SwitchDirs.Add(path);
		_store.Save();
		var games = SwitchGames.Scan(SwitchRoots());
		_novas.Load(games);
		ShowToast(games.Any(g => g.Path.StartsWith(path, StringComparison.OrdinalIgnoreCase)) ? "Juegos de Switch 2 añadidos" : "No se encontraron juegos de Switch 2 en esa carpeta");
	}

	private void GameExited()
	{
		if (_runningGame is { } game)
		{
			_store.PlaySeconds[game.TitleId] = _store.PlaySeconds.GetValueOrDefault(game.TitleId) + (DateTime.UtcNow - _started).TotalSeconds;
			_store.LastPlayed[game.TitleId]  = DateTime.UtcNow;
			_store.Save();
		}
		var closed   = _runningGame;
		var crashed  = _running is { } process && process.ExitCode != 0;
		_trophyCache.Clear(); // It may have earned trophies.
		_running     = null;
		_runningGame = null;
		WindowState  = WindowState.FullScreen;
		Activate();
		Select(_index, force: true);
		if (_tab == NovaSTab) _novas.Load(SwitchGames.Scan(SwitchRoots()));
		if (closed is not null) AfterArchiveTest(closed);
		if (crashed && closed is not null && _zone != Zone.Choice)
			ShowToast($"{closed.Name} se cerró por un error. El motivo queda en _Logs\\{closed.TitleId}.txt");
	}

	// ---- Building blocks ----------------------------------------------------------------------

	private Border Tile(Game game)
	{
		var tile = NewTile();
		if (game.Package is { } package)
		{
			tile.Background = new LinearGradientBrush
			{
				StartPoint = Palette.P(0, 0), EndPoint = Palette.P(1, 1),
				GradientStops = { new GradientStop(Color.Parse("#12324A"), 0), new GradientStop(Color.Parse("#2A1F5C"), 1) },
			};
			var panel = new StackPanel { Margin = new Thickness(14), VerticalAlignment = VerticalAlignment.Bottom, Spacing = 6 };
			panel.Children.Add(Text(Archives.IsArchive(package) ? Archives.Label(package) : "PKG", 16, FontWeight.Bold, Palette.AuroraA));
			panel.Children.Add(new TextBlock { Text = game.Name, FontSize = 17, FontWeight = FontWeight.SemiBold, TextWrapping = TextWrapping.Wrap, MaxLines = 3, Foreground = Palette.Brush(Palette.Text) });
			panel.Children.Add(new TextBlock { Text = _packageStatus.GetValueOrDefault(package, "Pendiente"), FontSize = 14, TextWrapping = TextWrapping.Wrap, MaxLines = 2, Foreground = Palette.Brush(Palette.Muted) });
			var bar = ProgressLine(double.NaN);
			ShowProgress(bar, _packageProgress.GetValueOrDefault(package));
			panel.Children.Add(bar);
			tile.Child = panel;
			return tile;
		}
		tile.Background = new LinearGradientBrush
		{
			StartPoint = Palette.P(0, 0), EndPoint = Palette.P(1, 1),
			GradientStops = { new GradientStop(Color.Parse("#1B2150"), 0), new GradientStop(Color.Parse("#2A1F5C"), 1) },
		};
		tile.Child = new TextBlock
		{
			Text = game.Name, FontSize = 20, FontWeight = FontWeight.SemiBold, TextWrapping = TextWrapping.Wrap,
			Margin = new Thickness(16), VerticalAlignment = VerticalAlignment.Bottom, Foreground = Palette.Brush(Palette.Text),
		};
		if (game.Icon is { } icon)
		{
			_ = LoadImage(icon, 256).ContinueWith(t =>
			{
				if (t.Result is { } bitmap)
				{
					tile.Background = new ImageBrush(bitmap) { Stretch = Stretch.UniformToFill };
					tile.Child      = null;
				}
			}, TaskScheduler.FromCurrentSynchronizationContext());
		}
		return tile;
	}

	private Border AddTile()
	{
		var tile = NewTile();
		tile.Background = Palette.Brush(Colors.White, 0.06);
		tile.Child = new TextBlock { Text = "+", FontSize = 64, FontWeight = FontWeight.Light, Foreground = Palette.Brush(Palette.Muted), HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
		return tile;
	}

	private static Border NewTile() => new()
	{
		Width = TileSize, Height = TileSize, CornerRadius = new CornerRadius(22), ClipToBounds = true, Opacity = 0.82,
		BorderBrush = Palette.Brush(Colors.White, 0.12), BorderThickness = new Thickness(1), VerticalAlignment = VerticalAlignment.Top,
		Transitions = new Transitions
		{
			new DoubleTransition { Property = WidthProperty, Duration = TimeSpan.FromMilliseconds(180) },
			new DoubleTransition { Property = HeightProperty, Duration = TimeSpan.FromMilliseconds(180) },
			new DoubleTransition { Property = OpacityProperty, Duration = TimeSpan.FromMilliseconds(180) },
		},
	};

	private static void Focus(Border tile, bool focused)
	{
		tile.Width           = tile.Height = focused ? FocusSize : TileSize;
		tile.Opacity         = focused ? 1 : 0.82;
		tile.BorderBrush     = Palette.Brush(Colors.White, focused ? 1 : 0.12);
		tile.BorderThickness = new Thickness(focused ? 3 : 1);
		tile.BoxShadow       = focused ? BoxShadows.Parse("0 0 0 6 #595EEAD4, 0 20 60 0 #99000000") : default;
	}

	private static Border ActionButton(string label, bool primary = false) => new()
	{
		Padding = new Thickness(38, 20), CornerRadius = new CornerRadius(18),
		Background = primary ? Palette.Aurora() : Palette.Brush(Colors.White, 0.1),
		BorderBrush = Palette.Brush(Colors.White, 0.18), BorderThickness = new Thickness(primary ? 0 : 1),
		Child = Text(label, 26, FontWeight.SemiBold, primary ? Color.Parse("#081022") : Palette.Text),
		Tag = primary,
	};

	private static void FocusButtonVisual(Border button, bool focused)
	{
		button.BorderBrush     = focused ? Brushes.White : Palette.Brush(Colors.White, 0.18);
		button.BorderThickness = new Thickness(focused ? 3 : (button.Tag is true ? 0 : 1));
		button.BoxShadow       = focused ? BoxShadows.Parse("0 0 0 5 #595EEAD4") : default;
	}

	private void ShowToast(string message)
	{
		if (_tab == NovaSTab)
		{
			_novas.Toast(message); // NovaS covers the PS5 stage, so it shows its own.
			return;
		}
		_toast.Text       = message;
		_toastBox.Opacity = 1;
		DispatcherTimer.RunOnce(() => _toastBox.Opacity = 0, TimeSpan.FromSeconds(3.5));
	}

	private static Image NewBackground() => new()
	{
		Stretch = Stretch.UniformToFill, Opacity = 0,
		Transitions = new Transitions { new DoubleTransition { Property = OpacityProperty, Duration = TimeSpan.FromMilliseconds(600) } },
	};

	private static TextBlock Text(string text, double size, FontWeight weight, Color? color = null) => new()
	{
		Text = text, FontSize = size, FontWeight = weight, Foreground = Palette.Brush(color ?? Palette.Text), VerticalAlignment = VerticalAlignment.Center,
	};

	private static Border Pill(Control child) => new()
	{
		Child = child, Padding = new Thickness(18, 8), CornerRadius = new CornerRadius(999),
		Background = Palette.Brush(Colors.White, 0.08), BorderBrush = Palette.Brush(Colors.White, 0.14), BorderThickness = new Thickness(1),
	};

	private static Border Badge(string text) => new()
	{
		Padding = new Thickness(14, 6), CornerRadius = new CornerRadius(10),
		Background = Palette.Brush(Palette.AuroraA, 0.16), BorderBrush = Palette.Brush(Palette.AuroraA, 0.4), BorderThickness = new Thickness(1),
		Child = Text(text, 18, FontWeight.SemiBold, Palette.AuroraA),
	};

	private static Control Hint(string key, string label)
	{
		var panel = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
		panel.Children.Add(new Border
		{
			MinWidth = 34, Height = 34, Padding = new Thickness(8, 0), CornerRadius = new CornerRadius(17),
			BorderBrush = Palette.Brush(Palette.Muted), BorderThickness = new Thickness(2),
			Child = new TextBlock { Text = key, FontSize = 15, FontWeight = FontWeight.Bold, Foreground = Palette.Brush(Palette.Text), HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center },
		});
		panel.Children.Add(Text(label, 20, FontWeight.Normal, Palette.Muted));
		return panel;
	}

	public static Control Logo(double size)
	{
		var canvas = new Canvas { Width = 64, Height = 64 };
		canvas.Children.Add(new Ellipse { Width = 56, Height = 56, Stroke = Palette.Aurora(), StrokeThickness = 4, [Canvas.LeftProperty] = 4, [Canvas.TopProperty] = 4 });
		canvas.Children.Add(new Avalonia.Controls.Shapes.Path { Data = Geometry.Parse("M21,44 V20 L43,44 V20"), Stroke = Brushes.White, StrokeThickness = 5, StrokeLineCap = PenLineCap.Round, StrokeJoin = PenLineJoin.Round });
		canvas.Children.Add(new Ellipse { Width = 10, Height = 10, Fill = Palette.Brush(Palette.AuroraA), [Canvas.LeftProperty] = 45, [Canvas.TopProperty] = 8 });
		return new Viewbox { Width = size, Height = size, Child = canvas };
	}

	// --snapshot <file.png>: renders the home screen at 1920x1080 and exits, to review the UI without a session.
	public static string? SnapshotPath { get; set; }
	public static bool    SnapshotNotes { get; set; }
	public static bool    SnapshotSettings { get; set; }
	public static bool    SnapshotGame { get; set; }
	public static bool    SnapshotTrophies { get; set; }
	public static bool    SnapshotNovaS { get; set; }
	public static bool    SnapshotNovaSSettings { get; set; }
	public static bool    SnapshotTrophyList { get; set; }

	private void SaveSnapshot(string path)
	{
		if (Content is Control root)
		{
			root.Measure(new Size(1920, 1080));
			root.Arrange(new Rect(0, 0, 1920, 1080));
			using var bitmap = new RenderTargetBitmap(new PixelSize(1920, 1080));
			bitmap.Render(root);
#pragma warning disable CS0618 // The replacement overload is not needed for a plain PNG.
			bitmap.Save(path);
#pragma warning restore CS0618
		}
		_music.Stop();
		Close();
	}

	private static void Place(Panel parent, Control child, double x, double y)
	{
		Canvas.SetLeft(child, x);
		Canvas.SetTop(child, y);
		if (parent is not Canvas)
		{
			child.HorizontalAlignment = HorizontalAlignment.Left;
			child.VerticalAlignment   = VerticalAlignment.Top;
			child.Margin              = new Thickness(x, y, 0, 0);
		}
		parent.Children.Add(child);
	}
}
