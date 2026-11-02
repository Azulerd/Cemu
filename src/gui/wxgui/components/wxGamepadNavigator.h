#pragma once

#include <wx/event.h>
#include <wx/timer.h>
#include <wx/stopwatch.h>
#include <wx/weakref.h>
#include <wx/window.h>

#include "wxgui/components/wxGamepadState.h"

// Lets the controller drive the regular Cemu windows (general settings, graphic packs, game profile, title manager...).
// One object for the whole application: every 33 ms it looks at the active window and, if it is one of those windows,
// turns the controller buttons into focus movement and control actions. No window needs to know about it.
//
//   d-pad / left stick  up, down: previous / next control      left, right: change the value of the focused control
//   accept button       press a button, toggle a check box, next value of a choice
//   back button         close the window (same as the X)
//   L / R               previous / next tab (or previous / next control when the window has no tabs)
//   Y and +             next / previous control (the way out of lists and trees, where up / down move inside them)
//
// Text boxes still need the keyboard. The input and hotkey windows are left alone on purpose (they listen to the controller
// themselves while a button is being assigned).
class wxGamepadNavigator : public wxEvtHandler
{
public:
	static void Start(); // call once, after the main window exists

private:
	wxGamepadNavigator();
	~wxGamepadNavigator() override = default;

	void OnTimer(wxTimerEvent& event);
	void Poll();
	wxWindow* FindTarget() const;
	void Act(wxWindow* target, const wxGamepadState& pad, bool up, bool down, bool left, bool right);
	void MoveFocus(wxWindow* target, bool forward);
	void ChangeTab(wxWindow* target, wxWindow* focus, bool forward);
	void ChangeValue(wxWindow* control, int direction);   // left / right on the focused control
	void Accept(wxWindow* control);
	void ShowFocusRects(wxWindow* target);

	wxTimer m_timer;
	wxStopWatch m_watch;
	wxWeakRef<wxWindow> m_target;
	wxGamepadState m_prev;
	bool m_waitRelease = true; // a window just appeared: the button that opened it must be released first
	bool m_focusRectsShown = false;
	long m_nextUp = 0, m_nextDown = 0, m_nextLeft = 0, m_nextRight = 0;
};
