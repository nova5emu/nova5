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
					if (Map((SDL_GamepadButton)e.gbutton.button) is { } input) Press(input);
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
		StickDirection();
		if (_held is { } repeat && DateTime.UtcNow >= _nextRepeat)
		{
			_nextRepeat = DateTime.UtcNow.AddMilliseconds(110);
			Input?.Invoke(repeat);
		}
	}

	private PadInput? _stick;

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
