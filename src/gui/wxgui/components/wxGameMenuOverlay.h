#pragma once

#include <wx/frame.h>
#include <wx/panel.h>
#include <wx/timer.h>
#include <wx/stopwatch.h>
#include <wx/weakref.h>

#include <functional>
#include <vector>

#include "wxgui/components/wxGameTileView.h"

// In-game menu in the style of the library options panel.
// Opened with the - and + buttons of the Wii U controller (as translated by the Cemu input mapping), it pauses the emulation
// and slides a panel in from the right edge of the game window. It is made of two borderless floating windows owned by the
// main window (one darkens the game, the other one is the panel), because while the game is paused no new frames are
// presented, so nothing can be drawn inside the render surface.
// Navigation: d-pad or left stick = move, accept / back buttons = see kAcceptIsWiiUB in wxGamepadConfig.h
class wxGameMenuOverlay : public wxEvtHandler
{
public:
	explicit wxGameMenuOverlay(wxFrame* owner);
	~wxGameMenuOverlay() override;

	// supplied by the main window
	std::function<std::vector<wxGameTileMenuItem>()> onFetchMenu; // top level menus of the classic menu bar
	std::function<void(int)> onMenuCommand;                       // runs a menu entry
	std::function<bool(int)> isKeepOpenCommand;                   // true for check / radio entries (the panel stays open)
	std::function<void()> onOpenSettings;
	std::function<void()> onBackToLibrary;                        // runs after the panel's own confirmation page
	std::function<void()> onQuit;                                 // same, "Salir de Cemu"
	std::function<bool()> isSoundEnabled;
	std::function<int()> getDisplayMode;                          // 0 = window, 1 = fullscreen, 2 = fullscreen without border
	std::function<void(int)> setDisplayMode;                      // changes it (the "Pantalla" rows of the panel)
	std::function<int()> getButtonStyle;                          // button layout of the hints: 0 auto, 1 PlayStation, 2 Xbox, 3 Nintendo
	std::function<void(int)> setButtonStyle;
	std::function<void()> takeScreenshot;                         // quick actions of the "Rápido" section
	std::function<bool()> getFastForward;
	std::function<void(bool)> setFastForward;
	std::function<int()> getVolume;                               // 0-100
	std::function<void(int)> setVolume;
	std::function<void()> onHidden;                               // the panel is gone, give the focus back to the game

	void Start(); // starts polling the controllers
	bool IsOpen() const { return m_state != State::Hidden; }
	void Open();
	void Close(); // slides out and resumes the game

private:
	enum class State { Hidden, Opening, Open, Closing };

	struct PadState
	{
		bool minus = false;
		bool plus = false;
		bool up = false;
		bool down = false;
		bool left = false;
		bool right = false;
		bool accept = false;
		bool back = false;
		bool AnyDown() const { return minus || plus || up || down || left || right || accept || back; }
	};

	void OnTimer(wxTimerEvent& event);
	void Poll();
	void Animate();
	bool CanOpen() const;
	void EnsureWindows();
	void LayoutWindows(double amount);
	int PanelWidth(const wxSize& client) const;
	void HideNow();                              // hides the windows right away, the game stays paused
	void RunHidden(const std::function<void()>& action); // hides, runs the action (it may open dialogs), then resumes the game
	void RequestResume();
	void ResumeNow();
	void StartAnimation();
	static PadState ReadPads();

	wxFrame* m_owner;
	wxWeakRef<wxFrame> m_dimFrame;
	wxWeakRef<wxFrame> m_panelFrame;
	wxWeakRef<wxPanel> m_clip;
	wxWeakRef<wxGameTileView> m_view;

	wxTimer m_pollTimer;
	wxTimer m_animTimer;
	wxStopWatch m_watch;

	State m_state = State::Hidden;
	double m_amount = 0.0; // 0 = hidden, 1 = fully open
	long m_lastAnimMs = 0;
	wxRect m_lastRect;

	// what the windows were last laid out with: they are only touched when something really changed
	wxRect m_laidOutRect;
	int m_laidOutPanelW = -1;
	int m_laidOutOffset = 0;
	int m_laidOutDimAlpha = -1;
	int m_laidOutPanelAlpha = -1;
	bool m_panelLayered = false; // the panel window fades in together with the slide (needs transparency support)
	long m_startMs = 0;
	bool m_prewarmed = false;    // the windows are created shortly after the game starts, not on the first opening

	bool m_paused = false;        // the emulation is paused by this overlay
	bool m_pendingResume = false; // waiting for the buttons to be released before the game gets control again
	long m_resumeRequestMs = 0;

	long m_chordSince = 0;        // when - and + were both pressed (0 = not pressed)
	bool m_waitRelease = false;   // ignore the controller until every button was released (the opening press)
	PadState m_prev;
	long m_nextRepeatUp = 0;
	long m_nextRepeatDown = 0;
	long m_nextRepeatLeft = 0;
	long m_nextRepeatRight = 0;
};
