#pragma once

#include <wx/frame.h>
#include <wx/timer.h>
#include <wx/stopwatch.h>
#include <wx/bitmap.h>
#include <wx/image.h>
#include <wx/string.h>

#include <memory>

// Shown over the main window from the moment a game is started from the library until Cemu draws something of its own
// (its shader loading screen, which uses the same game art, or the first frame of the game). It fills the black gap
// while the window and the renderer are being set up. A borderless floating window owned by the main window.
// It closes by itself (fading out), on a click, or after a safety timeout.
class wxGameLoadSplash : public wxFrame
{
public:
	wxGameLoadSplash(wxFrame* owner, const wxString& name, const wxBitmap& icon, const std::shared_ptr<const wxImage>& art);
	~wxGameLoadSplash() override;

	void Dismiss(); // fades out and closes

private:
	void OnPaint(wxPaintEvent& event);
	void OnTimer(wxTimerEvent& event);
	void OnClick(wxMouseEvent& event);
	void Reposition();
	void SetAlpha(int alpha);

	wxFrame* m_owner;
	wxString m_name;
	wxBitmap m_icon;
	wxBitmap m_art;       // 16:9 game art, null if the game has none
	wxTimer m_timer;
	wxStopWatch m_watch;
	wxRect m_lastRect;
	bool m_closing = false;
	bool m_showHint = false; // after fading out normally (not after a failed launch), the tip about the in-game menu is shown
	long m_closeStartMs = 0;
	int m_alpha = -1;
	bool m_transparentOk = false;
};

// Small tip shown over the game for a few seconds once it is running: how to open the in-game menu.
// A floating borderless window owned by the main window; mouse clicks pass through it (Windows) and it closes by itself.
// It waits (hidden) until the game draws its own frames, so the 5 seconds are not spent on the shader loading screen.
class wxGameHintToast : public wxFrame
{
public:
	explicit wxGameHintToast(wxFrame* owner);
	~wxGameHintToast() override;

private:
	void OnPaint(wxPaintEvent& event);
	void OnTimer(wxTimerEvent& event);
	void Reposition();
	void SetAlpha(int alpha);

	wxFrame* m_owner;
	wxString m_text;
	wxSize m_boxSize;
	wxTimer m_timer;
	wxStopWatch m_watch;
	long m_startMs = -1; // when the tip became visible (-1 = still waiting for the game)
	wxRect m_lastRect;
	int m_alpha = -1;
	bool m_transparentOk = false;
};
