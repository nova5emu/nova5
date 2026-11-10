using SDL;
using static SDL.SDL3;

namespace Nova5.Services;

public enum PadInput { Up, Down, Left, Right, Accept, Back, Options, Menu, PrevTab, NextTab }

// SDL gamepad input, polled from the UI thread. Sticks and d-pad repeat while held.
public sealed unsafe class Gamepad : IDisposable
{
	private readonly Dictionary<SDL_JoystickID, IntPtr> _pads = new();
	private PadInput? _held;
	private DateTime  _nextRepeat;

	public event Action<PadInput>? Input;
	public event Action<string?>? Connected;
	// Every button of the pad by its place on an Xbox-style pad ("south" is the bottom face button), for
	// Ajustes > Configurar mando. While Capture is on, buttons go only here and do not move the menus.
	public event Action<string>? Raw;
	public bool Capture { get; set; }

	// The pad buttons by place, with SDL's button for each (the triggers are axes, see Held).
	public static readonly (string Name, SDL_GamepadButton Button)[] PadButtons =
	{
		("south", SDL_GamepadButton.SDL_GAMEPAD_BUTTON_SOUTH), ("east", SDL_GamepadButton.SDL_GAMEPAD_BUTTON_EAST),
		("west", SDL_GamepadButton.SDL_GAMEPAD_BUTTON_WEST), ("north", SDL_GamepadButton.SDL_GAMEPAD_BUTTON_NORTH),
		("lb", SDL_GamepadButton.SDL_GAMEPAD_BUTTON_LEFT_SHOULDER), ("rb", SDL_GamepadButton.SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER),
		("start", SDL_GamepadButton.SDL_GAMEPAD_BUTTON_START), ("back", SDL_GamepadButton.SDL_GAMEPAD_BUTTON_BACK),
		("ls", SDL_GamepadButton.SDL_GAMEPAD_BUTTON_LEFT_STICK), ("rs", SDL_GamepadButton.SDL_GAMEPAD_BUTTON_RIGHT_STICK),
		("up", SDL_GamepadButton.SDL_GAMEPAD_BUTTON_DPAD_UP), ("down", SDL_GamepadButton.SDL_GAMEPAD_BUTTON_DPAD_DOWN),
		("left", SDL_GamepadButton.SDL_GAMEPAD_BUTTON_DPAD_LEFT), ("right", SDL_GamepadButton.SDL_GAMEPAD_BUTTON_DPAD_RIGHT),
	};

	private bool _leftTrigger, _rightTrigger;

	public bool HasPad => _pads.Count > 0;

	// What is held right now on any pad, and where the sticks point (-1..1).
	public HashSet<string> Held(out (double X, double Y) left, out (double X, double Y) right)
	{
		var held = new HashSet<string>();
		left = right = default;
		foreach (var handle in _pads.Values)
		{
			var pad = (SDL_Gamepad*)handle;
			foreach (var (name, button) in PadButtons)
				if (SDL_GetGamepadButton(pad, button)) held.Add(name);
			if (SDL_GetGamepadAxis(pad, SDL_GamepadAxis.SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 12000) held.Add("lt");
			if (SDL_GetGamepadAxis(pad, SDL_GamepadAxis.SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 12000) held.Add("rt");
			left  = (SDL_GetGamepadAxis(pad, SDL_GamepadAxis.SDL_GAMEPAD_AXIS_LEFTX) / 32767.0, SDL_GetGamepadAxis(pad, SDL_GamepadAxis.SDL_GAMEPAD_AXIS_LEFTY) / 32767.0);
			right = (SDL_GetGamepadAxis(pad, SDL_GamepadAxis.SDL_GAMEPAD_AXIS_RIGHTX) / 32767.0, SDL_GetGamepadAxis(pad, SDL_GamepadAxis.SDL_GAMEPAD_AXIS_RIGHTY) / 32767.0);
		}
		return held;
	}

	// The letter or symbol printed on the pad's face button at that place: A/B/X/Y on Xbox-style pads,
	// the shapes on PlayStation ones, Nintendo's letters on Switch pads.
	public string FaceLabel(string place)
	{
		var button = PadButtons.First(b => b.Name == place).Button;
		if (_pads.Count > 0)
		{
			switch (SDL_GetGamepadButtonLabel((SDL_Gamepad*)_pads.Values.First(), button))
			{
				case SDL_GamepadButtonLabel.SDL_GAMEPAD_BUTTON_LABEL_A: return "A";
				case SDL_GamepadButtonLabel.SDL_GAMEPAD_BUTTON_LABEL_B: return "B";
				case SDL_GamepadButtonLabel.SDL_GAMEPAD_BUTTON_LABEL_X: return "X";
				case SDL_GamepadButtonLabel.SDL_GAMEPAD_BUTTON_LABEL_Y: return "Y";
				case SDL_GamepadButtonLabel.SDL_GAMEPAD_BUTTON_LABEL_CROSS: return "✕";
				case SDL_GamepadButtonLabel.SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return "○";
				case SDL_GamepadButtonLabel.SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return "□";
				case SDL_GamepadButtonLabel.SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return "△";
			}
		}
		return place switch { "south" => "A", "east" => "B", "west" => "X", _ => "Y" };
	}

	public static bool Initialize()
	{
		// SDL has no window of its own here; Poll(active) decides from the Avalonia window instead.
		SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
		return SDL_Init(SDL_InitFlags.SDL_INIT_GAMEPAD | SDL_InitFlags.SDL_INIT_AUDIO);
	}

	public void Poll(bool active)
	{
		SDL_Event e;
		while (SDL_PollEvent(&e))
		{
			switch ((SDL_EventType)e.type)
			{
				case SDL_EventType.SDL_EVENT_GAMEPAD_ADDED:
					var pad = SDL_OpenGamepad(e.gdevice.which);
					if (pad is not null)
					{
						_pads[e.gdevice.which] = (IntPtr)pad;
						Connected?.Invoke(SDL_GetGamepadName(pad));
					}
					break;
				case SDL_EventType.SDL_EVENT_GAMEPAD_REMOVED:
					if (_pads.Remove(e.gdevice.which, out var removed)) SDL_CloseGamepad((SDL_Gamepad*)removed);
					Connected?.Invoke(_pads.Count > 0 ? SDL_GetGamepadName((SDL_Gamepad*)_pads.Values.First()) : null);
					break;
				case SDL_EventType.SDL_EVENT_GAMEPAD_BUTTON_DOWN when active:
					foreach (var (name, button) in PadButtons)
						if (button == (SDL_GamepadButton)e.gbutton.button) Raw?.Invoke(name);
					if (!Capture && Map((SDL_GamepadButton)e.gbutton.button) is { } input) Press(input);
					break;
				case SDL_EventType.SDL_EVENT_GAMEPAD_AXIS_MOTION when active:
					Trigger(ref _leftTrigger, SDL_GamepadAxis.SDL_GAMEPAD_AXIS_LEFT_TRIGGER, "lt", e.gaxis);
					Trigger(ref _rightTrigger, SDL_GamepadAxis.SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, "rt", e.gaxis);
					break;
				case SDL_EventType.SDL_EVENT_GAMEPAD_BUTTON_UP:
					if (Map((SDL_GamepadButton)e.gbutton.button) == _held) _held = null;
					break;
			}
		}
		if (!active)
		{
			_held = null;
			return;
		}
		if (Capture)
		{
			_held = null;
			return;
		}
		StickDirection();
		if (_held is { } repeat && DateTime.UtcNow >= _nextRepeat)
		{
			_nextRepeat = DateTime.UtcNow.AddMilliseconds(110);
			Input?.Invoke(repeat);
		}
	}

	private PadInput? _stick;

	// A trigger counts as a button press when it goes past halfway.
	private void Trigger(ref bool down, SDL_GamepadAxis axis, string name, SDL_GamepadAxisEvent e)
	{
		if ((SDL_GamepadAxis)e.axis != axis) return;
		var now = e.value > 16000;
		if (now && !down) Raw?.Invoke(name);
		down = now;
	}

	private void StickDirection()
	{
		PadInput? direction = null;
		foreach (var pad in _pads.Values)
		{
			int x = SDL_GetGamepadAxis((SDL_Gamepad*)pad, SDL_GamepadAxis.SDL_GAMEPAD_AXIS_LEFTX);
			int y = SDL_GetGamepadAxis((SDL_Gamepad*)pad, SDL_GamepadAxis.SDL_GAMEPAD_AXIS_LEFTY);
			if (Math.Abs(x) < 20000 && Math.Abs(y) < 20000) continue;
			direction = Math.Abs(x) > Math.Abs(y) ? (x > 0 ? PadInput.Right : PadInput.Left) : (y > 0 ? PadInput.Down : PadInput.Up);
		}
		if (direction == _stick) return;
		if (_stick is not null && _held == _stick) _held = null;
		_stick = direction;
		if (direction is { } d) Press(d);
	}

	private void Press(PadInput input)
	{
		Input?.Invoke(input);
		if (input is PadInput.Up or PadInput.Down or PadInput.Left or PadInput.Right)
		{
			_held       = input;
			_nextRepeat = DateTime.UtcNow.AddMilliseconds(380);
		}
	}

	private static PadInput? Map(SDL_GamepadButton button) => button switch
	{
		SDL_GamepadButton.SDL_GAMEPAD_BUTTON_DPAD_UP        => PadInput.Up,
		SDL_GamepadButton.SDL_GAMEPAD_BUTTON_DPAD_DOWN      => PadInput.Down,
		SDL_GamepadButton.SDL_GAMEPAD_BUTTON_DPAD_LEFT      => PadInput.Left,
		SDL_GamepadButton.SDL_GAMEPAD_BUTTON_DPAD_RIGHT     => PadInput.Right,
		SDL_GamepadButton.SDL_GAMEPAD_BUTTON_SOUTH          => PadInput.Accept,
		SDL_GamepadButton.SDL_GAMEPAD_BUTTON_EAST           => PadInput.Back,
		SDL_GamepadButton.SDL_GAMEPAD_BUTTON_NORTH          => PadInput.Options,
		SDL_GamepadButton.SDL_GAMEPAD_BUTTON_START          => PadInput.Menu,
		SDL_GamepadButton.SDL_GAMEPAD_BUTTON_LEFT_SHOULDER  => PadInput.PrevTab,
		SDL_GamepadButton.SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER => PadInput.NextTab,
		_ => null,
	};

	public void Dispose()
	{
		foreach (var pad in _pads.Values) SDL_CloseGamepad((SDL_Gamepad*)pad);
		_pads.Clear();
	}
}
