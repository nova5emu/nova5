#pragma once

#include <cstdint>
#include <utility>

namespace Libs::Graphics {

enum class GameBarAction { None, Fullscreen, Exit };

// Pure interaction state. The overlay serializes access; SDL and ImGui stay
// on their respective threads. Reopening gets a new input session.
class GameBarState {
public:
	static constexpr float Height = 60.0f;
	bool Visible() const { return m_mode != Mode::Hidden; }
	bool ConfirmingExit() const { return m_confirm; }
	uint64_t Revision() const { return m_revision; }
	uint64_t Session() const { return m_session; }

	void Block(bool blocked) {
		m_blocked = blocked;
		if (blocked) { Hide(); m_action = GameBarAction::None; }
	}
	void Hide() {
		if (Visible()) ++m_revision;
		m_mode = Mode::Hidden;
		m_confirm = false;
		m_edge_armed = false;
	}
	void Escape(bool repeat) {
		if (m_blocked || repeat) return;
		if (Visible()) Hide();
		else Show(Mode::Keyboard);
	}
	void Pointer(float y, bool inside) {
		if (!inside || y >= 6.0f) m_edge_armed = true;
		if (m_blocked) return;
		if (m_mode == Mode::Mouse && (!inside || y >= Height)) { Hide(); m_edge_armed = true; }
		else if (!Visible() && m_edge_armed && inside && y >= 0.0f && y < 6.0f) Show(Mode::Mouse);
	}
	void RequestExit() {
		if (Visible() && !m_blocked) { m_confirm = true; ++m_revision; }
	}
	void CancelExit() { m_confirm = false; ++m_revision; }
	void ConfirmExit() {
		if (Visible() && m_confirm && !m_blocked) m_action = GameBarAction::Exit;
	}
	void Fullscreen() {
		if (Visible() && !m_blocked) m_action = GameBarAction::Fullscreen;
	}
	GameBarAction TakeAction() { return std::exchange(m_action, GameBarAction::None); }

private:
	enum class Mode { Hidden, Mouse, Keyboard };
	void Show(Mode mode) { m_mode = mode; ++m_session; ++m_revision; }
	Mode m_mode = Mode::Hidden;
	bool m_blocked = false, m_confirm = false, m_edge_armed = true;
	uint64_t m_revision = 0, m_session = 0;
	GameBarAction m_action = GameBarAction::None;
};

} // namespace Libs::Graphics
