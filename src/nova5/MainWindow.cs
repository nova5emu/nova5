using System.Diagnostics;
using Avalonia;
using Avalonia.Animation;
using Avalonia.Controls;
using Avalonia.Controls.Shapes;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform.Storage;
using Avalonia.Threading;
using Nova5.Services;

namespace Nova5;

public sealed class MainWindow : Window
{
	private enum Zone { Tiles, Buttons, Settings, Prompt }

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
	private readonly StackPanel _settingsList = new() { Spacing = 10, Width = 1100 };
	private readonly Panel      _settingsPage = new() { IsVisible = false };
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
	private UpdateInfo?         _update;
	private bool                _updating;

	public MainWindow()
	{
		Title      = "Nova5";
		Background = Palette.Brush(Palette.Night);
		FontFamily = Palette.Font;
		Width      = 1600;
		Height     = 900;
		WindowState = SnapshotPath is null ? WindowState.FullScreen : WindowState.Normal;

		_buttons = new[] { ActionButton("Jugar", primary: true), ActionButton("Ajustes"), ActionButton("Abrir carpeta") };
		_tabs    = new[] { Text("Juegos", 24, FontWeight.SemiBold), Text("Ajustes", 24, FontWeight.Normal, Palette.Muted) };
		_toastBox = new Border
		{
			Child = _toast, Padding = new Thickness(28, 14), CornerRadius = new CornerRadius(16), Opacity = 0,
			Background = Palette.Brush(Palette.Ink, 0.92), BorderBrush = Palette.Brush(Colors.White, 0.15), BorderThickness = new Thickness(1),
			Transitions = new Transitions { new DoubleTransition { Property = OpacityProperty, Duration = TimeSpan.FromMilliseconds(250) } },
		};
		_prompt = BuildPrompt();
		Content = BuildLayout();

		_music.Enabled = _store.Music;
		_music.Volume  = _store.MusicVolume;
		if (!Gamepad.Initialize()) _padStatus.Text = "Mando no disponible";
		_pad.Input     += OnInput;
		_pad.Connected += name => _padStatus.Text = name is null ? "Sin mando" : $"Mando: {name}";

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
			if (_running is not null || _zone == Zone.Prompt) return;
			OnInput(e.Delta.Y < 0 ? (_tab == 0 ? PadInput.Right : PadInput.Down) : (_tab == 0 ? PadInput.Left : PadInput.Up));
		};
		PropertyChanged += (_, e) =>
		{
			if (e.Property == WindowStateProperty) _fullscreenLabel.Text = WindowState == WindowState.FullScreen ? "Ventana" : "Pantalla completa";
		};
		Opened  += (_, _) =>
		{
			Reload();
			_fullscreenLabel.Text = WindowState == WindowState.FullScreen ? "Ventana" : "Pantalla completa";
			if (SnapshotPath is { } snapshot) DispatcherTimer.RunOnce(() => SaveSnapshot(snapshot), TimeSpan.FromSeconds(4));
			else if (_store.UpdateMode != "no") DispatcherTimer.RunOnce(() => CheckUpdates(false), TimeSpan.FromSeconds(3));
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
		Place(stage, brand, 72, 34);

		var status = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 28, VerticalAlignment = VerticalAlignment.Center };
		_updatePill = Pill(_updateText);
		_updatePill.IsVisible   = false;
		_updatePill.BorderBrush = Palette.Aurora();
		_updatePill.Cursor      = new Cursor(StandardCursorType.Hand);
		_updatePill.PointerPressed += (_, _) => StartUpdate();
		status.Children.Add(_updatePill);
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

		var statsBox = _statsBox = new Border
		{
			Width = 440, Padding = new Thickness(30, 24), CornerRadius = new CornerRadius(24),
			Background = Palette.Brush(Palette.Ink, 0.88), BorderBrush = Palette.Brush(Colors.White, 0.12), BorderThickness = new Thickness(1),
			Child = _stats,
		};
		Place(_gamesPage, statsBox, 1920 - 72 - 440, 650);
		stage.Children.Add(_gamesPage);

		// Settings page.
		var settingsTitle = Text("Ajustes del emulador", 44, FontWeight.Bold);
		Place(_settingsPage, settingsTitle, 72, 200);
		Place(_settingsPage, _settingsList, 72, 290);
		stage.Children.Add(_settingsPage);

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

		var root = new Grid { Background = Palette.Brush(Palette.Night) };
		root.Children.Add(_backA);
		root.Children.Add(_backB);
		root.Children.Add(new Rectangle { Fill = Palette.Fade(Palette.P(0, 0), Palette.P(1, 0), (0, 0.92), (0.38, 0.55), (0.70, 0)) });
		root.Children.Add(new Rectangle { Fill = Palette.Fade(Palette.P(0, 1), Palette.P(0, 0), (0, 0.95), (0.45, 0)) });
		root.Children.Add(new Rectangle { Fill = Palette.Fade(Palette.P(0, 0), Palette.P(0, 1), (0, 0.7), (0.18, 0)) });
		root.Children.Add(new Viewbox { Stretch = Stretch.Uniform, Child = stage });
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

	// ---- Data ---------------------------------------------------------------------------------

	private void Reload()
	{
		var roots = _emulator.GameDirs().Concat(_store.ExtraDirs).Distinct(StringComparer.OrdinalIgnoreCase).ToList();
		var previous = _games.ElementAtOrDefault(_index)?.Dir;
		_games = Library.Scan(roots);
		_index = Math.Max(0, _games.FindIndex(g => g.Dir == previous));
		_row.Children.Clear();
		foreach (var game in _games) _row.Children.Add(Tile(game));
		_row.Children.Add(AddTile());
		for (var i = 0; i < _row.Children.Count; i++)
		{
			var index = i;
			_row.Children[i].Cursor = new Cursor(StandardCursorType.Hand);
			_row.Children[i].PointerPressed += (_, _) => TileClicked(index);
		}
		if (!_emulator.Exists) ShowToast("No se encontró Nova5Emulator junto al lanzador");
		Select(_index, force: true);
		WatchFolders(roots);
		InstallPendingPackages();
	}

	// ---- Packages -----------------------------------------------------------------------------

	private readonly Dictionary<string, string> _packageStatus = new(StringComparer.OrdinalIgnoreCase);
	private readonly HashSet<string>            _packageFailed = new(StringComparer.OrdinalIgnoreCase);
	private readonly List<FileSystemWatcher>    _watchers = new();
	private bool                                _installingPackages;

	// Installs every new .pkg in the game folders, one at a time, as soon as it has finished copying.
	private async void InstallPendingPackages()
	{
		if (_installingPackages) return;
		_installingPackages = true;
		try
		{
			while (_games.FirstOrDefault(g => g.Package is { } p && !_packageFailed.Contains(p)) is { Package: { } package } game)
			{
				SetPackageStatus(package, "Esperando a que termine de copiarse…");
				if (!await Packages.WaitUntilStableAsync(package, CancellationToken.None))
				{
					_packageFailed.Add(package);
					continue;
				}
				ShowToast($"Instalando {game.Name}…");
				try
				{
					var progress = new Progress<double>(p => SetPackageStatus(package, $"Instalando {p * 100:0} %"));
					await Task.Run(() => Packages.InstallAsync(package, progress, CancellationToken.None));
					_packageStatus.Remove(package);
					Reload();
					ShowToast($"{game.Name} está listo para jugar");
				}
				catch (Exception e) when (e is IOException or InvalidDataException or InvalidOperationException or UnauthorizedAccessException or System.ComponentModel.Win32Exception)
				{
					try { Packages.CleanUp(package); } catch (IOException) { }
					_packageFailed.Add(package);
					SetPackageStatus(package, "No se pudo instalar: " + e.Message);
					ShowToast($"No se pudo instalar {game.Name}");
				}
			}
		}
		finally
		{
			_installingPackages = false;
		}
	}

	private void SetPackageStatus(string package, string status)
	{
		_packageStatus[package] = status;
		var index = _games.FindIndex(g => g.Package == package);
		if (index < 0) return;
		if (_row.Children[index] is Border { Child: StackPanel panel } && panel.Children.LastOrDefault() is TextBlock label) label.Text = status;
		if (index == _index) Select(_index, force: true);
	}

	private void WatchFolders(IEnumerable<string> roots)
	{
		if (_watchers.Count > 0) return;
		foreach (var root in roots.Where(Directory.Exists))
		{
			var watcher = new FileSystemWatcher(root, "*.pkg") { IncludeSubdirectories = true, EnableRaisingEvents = true };
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
			_meta.Children.Add(Text("Elige la carpeta donde tienes tus juegos de PS5 extraídos", 22, FontWeight.Normal, Palette.Muted));
			SetBackground(null);
			_music.Stop();
			return;
		}
		if (game.Package is { } package)
		{
			foreach (var button in _buttons) button.IsVisible = false;
			if (_statsBox is not null) _statsBox.IsVisible = false;
			_meta.Children.Add(Badge("Paquete PKG"));
			_meta.Children.Add(Text($"{new FileInfo(package).Length / 1e9:0.0} GB", 22, FontWeight.Normal, Palette.Muted));
			_meta.Children.Add(Text(_packageStatus.GetValueOrDefault(package, "Pendiente de instalar"), 22, FontWeight.Normal, Palette.Muted));
			if (_packageFailed.Contains(package)) _meta.Children.Add(Text("· pulsa A para reintentar", 22, FontWeight.Normal, Palette.AuroraA));
			SetBackground(null);
			_music.Stop();
			return;
		}
		_meta.Children.Add(Badge(_emulator.PatchFor(game.TitleId) is null ? "Instalado" : "Parche activo"));
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
		if (_zone == Zone.Prompt)
		{
			if (input == PadInput.Accept) Close();
			if (input == PadInput.Back) ShowPrompt(false);
			return;
		}
		if (input is PadInput.PrevTab or PadInput.NextTab)
		{
			ShowTab(_tab == 0 ? 1 : 0);
			return;
		}
		if (_zone == Zone.Settings)
		{
			SettingsInput(input);
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
			case PadInput.Options or PadInput.Menu: ShowTab(1); break;
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
			case 1: ShowTab(1); break;
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
		if (_zone is Zone.Prompt or Zone.Settings || _running is not null) return;
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
		_tab = tab;
		for (var i = 0; i < _tabs.Length; i++)
		{
			_tabs[i].FontWeight = i == tab ? FontWeight.SemiBold : FontWeight.Normal;
			_tabs[i].Foreground = Palette.Brush(i == tab ? Palette.Text : Palette.Muted);
		}
		_gamesPage.IsVisible    = tab == 0;
		_settingsPage.IsVisible = tab == 1;
		if (tab == 1)
		{
			_zone = Zone.Settings;
			BuildSettings();
		}
		else
		{
			SetZone(Zone.Tiles);
		}
	}

	// ---- Settings -----------------------------------------------------------------------------

	private sealed record Setting(string Label, Func<string> Value, Action<int> Change);

	private static readonly string[] Resolutions = { "R1280X720", "R1920X1080", "R2560X1440", "R3840X2160" };

	private List<Setting> Settings() => new()
	{
		Toggle("Pantalla completa al jugar", "fullscreen_enabled", true),
		new("Resolución", () => _emulator.Ini.Get("GlobalConfiguration", "screen_resolution")?.TrimStart('R').Replace('X', '×') ?? "1920×1080",
		    step =>
		    {
			    var current = Array.IndexOf(Resolutions, _emulator.Ini.Get("GlobalConfiguration", "screen_resolution") ?? "R1920X1080");
			    SetIni("screen_resolution", Resolutions[(Math.Max(current, 0) + step + Resolutions.Length) % Resolutions.Length]);
		    }),
		Toggle("Contador de FPS en pantalla", "perf_overlay_enabled", false),
		Toggle("Caché de shaders (cargas más rápidas)", "shader_cache_enabled", true),
		Toggle("Compatibilidad de CPU (--amd-cpu)", "amd_cpu_enabled", false),
		new("Música del menú", () => _store.Music ? "Sí" : "No", _ =>
		{
			_store.Music   = !_store.Music;
			_music.Enabled = _store.Music;
			_store.Save();
		}),
		new("Volumen de la música", () => $"{_store.MusicVolume * 100:0} %", step =>
		{
			_store.MusicVolume = Math.Clamp(Math.Round(_store.MusicVolume + step * 0.1, 1), 0, 1);
			_music.Volume      = _store.MusicVolume;
			_store.Save();
		}),
		new("Actualizaciones", () => _store.UpdateMode switch { "auto" => "Automáticas", "no" => "Desactivadas", _ => "Avisar" }, step =>
		{
			string[] modes = { "avisar", "auto", "no" };
			_store.UpdateMode = modes[(Math.Max(Array.IndexOf(modes, _store.UpdateMode), 0) + step + modes.Length) % modes.Length];
			_store.Save();
		}),
		new(_update is null ? $"Buscar actualizaciones (tienes la {Updates.Current})" : $"Actualizar a Nova5 {_update.Version}",
		    () => _updating ? "Actualizando…" : "", _ => { if (_update is null) CheckUpdates(true); else StartUpdate(); }),
		new("Añadir carpeta de juegos", () => "", _ => AddFolder()),
		new("Salir de Nova5", () => "", _ => ShowPrompt(true)),
	};

	private Setting Toggle(string label, string key, bool fallback) =>
	    new(label, () => _emulator.Ini.GetBool("GlobalConfiguration", key, fallback) ? "Sí" : "No",
	        _ => SetIni(key, _emulator.Ini.GetBool("GlobalConfiguration", key, fallback) ? "false" : "true"));

	private void SetIni(string key, string value)
	{
		_emulator.Ini.Set("GlobalConfiguration", key, value);
		_emulator.Ini.Save();
	}

	private void BuildSettings()
	{
		_settingsList.Children.Clear();
		var settings = Settings();
		_setting = Math.Clamp(_setting, 0, settings.Count - 1);
		for (var i = 0; i < settings.Count; i++)
		{
			var row   = new DockPanel();
			var value = Text(settings[i].Value(), 26, FontWeight.SemiBold, Palette.AuroraA);
			DockPanel.SetDock(value, Dock.Right);
			row.Children.Add(value);
			row.Children.Add(Text(settings[i].Label, 26, FontWeight.Normal));
			var focused = i == _setting;
			var index   = i;
			var item = new Border
			{
				Child = row, Padding = new Thickness(28, 18), CornerRadius = new CornerRadius(16),
				Background = Palette.Brush(focused ? Colors.White : Palette.Ink, focused ? 0.14 : 0.55),
				BorderBrush = focused ? Palette.Aurora() : Palette.Brush(Colors.White, 0.08), BorderThickness = new Thickness(focused ? 2 : 1),
				Cursor = new Cursor(StandardCursorType.Hand),
			};
			// Left click goes to the next value, right click to the previous one.
			item.PointerPressed += (_, e) =>
			{
				if (_zone != Zone.Settings) return;
				_setting = index;
				Settings()[index].Change(e.GetCurrentPoint(item).Properties.IsRightButtonPressed ? -1 : 1);
				if (_zone == Zone.Settings && _tab == 1) BuildSettings();
			};
			_settingsList.Children.Add(item);
		}
	}

	private void SettingsInput(PadInput input)
	{
		var settings = Settings();
		switch (input)
		{
			case PadInput.Up:    _setting = Math.Max(0, _setting - 1); break;
			case PadInput.Down:  _setting = Math.Min(settings.Count - 1, _setting + 1); break;
			case PadInput.Left:  settings[_setting].Change(-1); break;
			case PadInput.Right or PadInput.Accept: settings[_setting].Change(1); break;
			case PadInput.Back:  ShowTab(0); return;
		}
		BuildSettings();
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
		if (_tab == 1) ShowTab(0);
	}

	// ---- Updates ------------------------------------------------------------------------------

	private async void CheckUpdates(bool manual)
	{
		if (manual) ShowToast("Buscando actualizaciones…");
		_update = await Updates.CheckAsync();
		if (_tab == 1) BuildSettings();
		if (_update is null)
		{
			if (manual) ShowToast($"Tienes la última versión ({Updates.Current})");
			return;
		}
		_updateText.Text      = $"Actualizar a {_update.Version}";
		_updatePill.IsVisible = true;
		if (_store.UpdateMode == "auto" && !manual) StartUpdate();
		else ShowToast($"Nova5 {_update.Version} está disponible: pulsa «Actualizar» arriba o en Ajustes");
	}

	private async void StartUpdate()
	{
		if (_update is not { } update || _updating) return;
		if (_running is not null)
		{
			ShowToast("Cierra el juego para actualizar Nova5");
			return;
		}
		_updating = true;
		_music.Stop();
		try
		{
			var progress = new Progress<double>(p => _updateText.Text = $"Descargando {p * 100:0} %");
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

	private void GameExited()
	{
		if (_runningGame is { } game)
		{
			_store.PlaySeconds[game.TitleId] = _store.PlaySeconds.GetValueOrDefault(game.TitleId) + (DateTime.UtcNow - _started).TotalSeconds;
			_store.LastPlayed[game.TitleId]  = DateTime.UtcNow;
			_store.Save();
		}
		_running     = null;
		_runningGame = null;
		WindowState  = WindowState.FullScreen;
		Activate();
		Select(_index, force: true);
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
			panel.Children.Add(Text("PKG", 16, FontWeight.Bold, Palette.AuroraA));
			panel.Children.Add(new TextBlock { Text = game.Name, FontSize = 17, FontWeight = FontWeight.SemiBold, TextWrapping = TextWrapping.Wrap, MaxLines = 3, Foreground = Palette.Brush(Palette.Text) });
			panel.Children.Add(new TextBlock { Text = _packageStatus.GetValueOrDefault(package, "Pendiente"), FontSize = 14, TextWrapping = TextWrapping.Wrap, MaxLines = 2, Foreground = Palette.Brush(Palette.Muted) });
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
