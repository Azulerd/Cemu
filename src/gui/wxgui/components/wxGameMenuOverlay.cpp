#include "wxgui/components/wxGameMenuOverlay.h"

#include <wx/app.h>
#include <wx/dcbuffer.h>

#include <algorithm>
#include <cmath>
#include <memory>

#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "input/InputManager.h"
#include "input/emulated/VPADController.h"
#include "input/emulated/ProController.h"
#include "input/emulated/ClassicController.h"
#include "input/emulated/WiimoteController.h"
#include "wxgui/components/wxGamepadConfig.h"

namespace
{
	constexpr int kPollTimerId = 1;
	constexpr int kAnimTimerId = 2;
	constexpr long kPollMs = 16;            // while the menu is open
	constexpr long kPollIdleMs = 33;        // while playing: the - + chord does not need a faster check
	constexpr long kPrewarmMs = 2500;       // the windows are created this long after the game started
	constexpr long kChordHoldMs = 120;      // - and + must be held together this long to open the menu
	constexpr long kRepeatDelayMs = 380;    // d-pad / stick auto repeat
	constexpr long kRepeatIntervalMs = 90;
	constexpr long kResumeTimeoutMs = 1500; // the game gets control again at the latest after this, even if a button is still held
	constexpr double kStickThreshold = 0.6;
	constexpr int kDimAlpha = 150;          // darkening of the game behind the panel (0-255)

	bool IsMinusDown(const EmulatedControllerPtr& c)
	{
		// the "-" button id is different in every emulated controller type
		if (const auto p = std::dynamic_pointer_cast<ProController>(c))
			return p->is_mapping_down(ProController::kButtonId_Minus);
		if (const auto p = std::dynamic_pointer_cast<ClassicController>(c))
			return p->is_mapping_down(ClassicController::kButtonId_Minus);
		if (const auto p = std::dynamic_pointer_cast<WiimoteController>(c))
			return p->is_mapping_down(WiimoteController::kButtonId_Minus);
		if (const auto p = std::dynamic_pointer_cast<VPADController>(c))
			return p->is_mapping_down(VPADController::kButtonId_Minus);
		return false;
	}
}

wxGameMenuOverlay::wxGameMenuOverlay(wxFrame* owner)
	: m_owner(owner), m_pollTimer(this, kPollTimerId), m_animTimer(this, kAnimTimerId)
{
	Bind(wxEVT_TIMER, &wxGameMenuOverlay::OnTimer, this);
}

wxGameMenuOverlay::~wxGameMenuOverlay()
{
	m_pollTimer.Stop();
	m_animTimer.Stop();
	// never leave the emulation paused behind (the windows are children of the owner and go away with it)
	if (m_paused)
		coreinit::OSSchedulerSetPaused(false);
}

void wxGameMenuOverlay::Start()
{
	m_startMs = std::max<long>(1, m_watch.Time());
	m_pollTimer.Start(kPollIdleMs);
}

void wxGameMenuOverlay::StartAnimation()
{
	if (m_animTimer.IsRunning())
		return;
	m_lastAnimMs = m_watch.Time();
	m_animTimer.Start(16);
}

void wxGameMenuOverlay::OnTimer(wxTimerEvent& event)
{
	if (event.GetId() == kPollTimerId)
		Poll();
	else if (event.GetId() == kAnimTimerId)
		Animate();
}

// every pressed button of every connected controller, as seen through the Cemu mapping (already translated to Wii U buttons)
wxGameMenuOverlay::PadState wxGameMenuOverlay::ReadPads()
{
	PadState state;
	auto& inputManager = InputManager::instance();
	for (size_t i = 0; i < InputManager::kMaxController; ++i)
	{
		const EmulatedControllerPtr controller = inputManager.get_controller(i);
		if (!controller)
			continue;
		// Cemu only refreshes the button state when the game reads the pad: with the game paused (or in the library) nobody does,
		// so the state would stay frozen. Refresh it here before reading.
		controller->controllers_update_states();
		state.plus |= controller->is_start_down();
		state.minus |= IsMinusDown(controller);
		const glm::vec2 axis = controller->get_axis(); // left stick
		state.up |= controller->is_up_down() || axis.y > kStickThreshold;
		state.down |= controller->is_down_down() || axis.y < -kStickThreshold;
		state.left |= controller->is_left_down() || axis.x < -kStickThreshold;
		state.right |= controller->is_right_down() || axis.x > kStickThreshold;
		const bool a = controller->is_a_down();
		const bool b = controller->is_b_down();
		state.accept |= kAcceptIsWiiUB ? b : a;
		state.back |= kAcceptIsWiiUB ? a : b;
	}
	return state;
}

bool wxGameMenuOverlay::CanOpen() const
{
	// a modal dialog disables the owner, don't open the menu on top of it
	return m_owner && m_owner->IsEnabled() && m_owner->IsShownOnScreen() && !m_owner->IsIconized() && wxTheApp && wxTheApp->IsActive();
}

void wxGameMenuOverlay::Poll()
{
	const long now = std::max<long>(1, m_watch.Time());
	const PadState pad = ReadPads();

	// after closing the menu the game only continues once the buttons were released, so it does not receive the press that closed it
	if (m_pendingResume && (!pad.AnyDown() || now - m_resumeRequestMs > kResumeTimeoutMs))
		ResumeNow();

	if (m_state == State::Hidden)
	{
		if (!m_prewarmed && m_startMs > 0 && now - m_startMs > kPrewarmMs && onFetchMenu)
		{
			m_prewarmed = true;
			EnsureWindows(); // creates the (hidden) windows ahead of time, so the first opening does not stutter
		}
		if (!m_paused && pad.minus && pad.plus)
		{
			if (m_chordSince == 0)
				m_chordSince = now;
			else if (now - m_chordSince >= kChordHoldMs && CanOpen())
			{
				m_chordSince = 0;
				Open();
			}
		}
		else
			m_chordSince = 0;
		m_prev = pad;
		return;
	}

	if (m_state == State::Closing || !m_view)
	{
		m_prev = pad;
		return;
	}

	// keep the windows glued to the game window if it was moved or resized while the menu is open
	if (m_state == State::Open)
	{
		const wxRect rect(m_owner->ClientToScreen(wxPoint(0, 0)), m_owner->GetClientSize());
		if (rect != m_lastRect)
			LayoutWindows(m_amount);
	}

	if (m_waitRelease)
	{
		if (!pad.AnyDown())
			m_waitRelease = false;
		m_prev = pad;
		return;
	}

	if (pad.accept && !m_prev.accept)
		m_view->Navigate(wxGameTileView::NavAction::Accept);
	else if (pad.back && !m_prev.back)
		m_view->Navigate(wxGameTileView::NavAction::Back);

	auto repeat = [&](bool down, bool wasDown, long& nextRepeat, wxGameTileView::NavAction action) {
		if (!down)
		{
			nextRepeat = 0;
			return;
		}
		if (!wasDown)
		{
			m_view->Navigate(action);
			nextRepeat = now + kRepeatDelayMs;
		}
		else if (now >= nextRepeat)
		{
			m_view->Navigate(action);
			nextRepeat = now + kRepeatIntervalMs;
		}
	};
	repeat(pad.up, m_prev.up, m_nextRepeatUp, wxGameTileView::NavAction::Up);
	repeat(pad.down, m_prev.down, m_nextRepeatDown, wxGameTileView::NavAction::Down);
	repeat(pad.left, m_prev.left, m_nextRepeatLeft, wxGameTileView::NavAction::Left);
	repeat(pad.right, m_prev.right, m_nextRepeatRight, wxGameTileView::NavAction::Right);

	m_prev = pad;
}

int wxGameMenuOverlay::PanelWidth(const wxSize& client) const
{
	// same proportions as the options panel of the library
	const double dpi = m_owner->GetDPIScaleFactor();
	return (int)std::lround(std::clamp(client.x * 0.30, 330.0 * dpi, 480.0 * dpi));
}

void wxGameMenuOverlay::EnsureWindows()
{
	if (m_panelFrame)
		return;

	const long style = wxFRAME_NO_TASKBAR | wxFRAME_FLOAT_ON_PARENT | wxBORDER_NONE;

	// darkens the game; a click on it closes the menu
	auto* dim = new wxFrame(m_owner, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(10, 10), style);
	if (dim->CanSetTransparent())
	{
		dim->SetBackgroundColour(*wxBLACK);
		dim->SetTransparent(0);
		dim->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) { Close(); });
		dim->Bind(wxEVT_RIGHT_UP, [this](wxMouseEvent&) { Close(); });
		m_dimFrame = dim;
	}
	else
		dim->Destroy(); // no transparency support here: the panel is shown without darkening

	// the panel: a frame whose width grows from the right edge, with the content aligned to the right
	auto* panel = new wxFrame(m_owner, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(10, 10), style);
	panel->SetBackgroundColour(wxColour(8, 10, 18));
	if (panel->CanSetTransparent())
	{
		panel->SetTransparent(0);
		m_panelLayered = true;
		m_laidOutPanelAlpha = 0;
	}
	auto* clip = new wxPanel(panel);
	clip->SetBackgroundColour(wxColour(8, 10, 18));
	auto* view = new wxGameTileView(clip);
	view->SetOverlayMode(true);
	view->onFetchMenu = onFetchMenu;
	view->onPanelClosed = [this]() { Close(); };
	view->onMenuCommand = [this](int id) {
		if (isKeepOpenCommand && isKeepOpenCommand(id))
		{
			// check / radio entries: the panel stays open and shows the new state
			if (onMenuCommand)
				onMenuCommand(id);
		}
		else
			RunHidden([this, id]() { if (onMenuCommand) onMenuCommand(id); });
	};
	if (setDisplayMode)
	{
		view->onGetDisplayMode = getDisplayMode;
		view->onSetDisplayMode = [this](int mode) {
			setDisplayMode(mode);
			// the game window changed size / position (and may have changed its place in the window stack): put the menu back on top of it
			CallAfter([this]() {
				if (m_state == State::Hidden || !m_panelFrame || !m_view)
					return;
				LayoutWindows(m_amount);
				if (m_dimFrame)
					m_dimFrame->Raise();
				m_panelFrame->Raise();
				m_view->SetFocus();
			});
		};
	}
	if (setButtonStyle)
	{
		view->onGetButtonStyle = getButtonStyle;
		view->onSetButtonStyle = setButtonStyle;
	}
	if (takeScreenshot)
		view->onScreenshot = [this]() { RunHidden(takeScreenshot); }; // the shot is taken once the game runs again, without the menu on top
	if (setFastForward)
	{
		view->onGetFastForward = getFastForward;
		view->onSetFastForward = setFastForward;
	}
	if (setVolume)
	{
		view->onGetVolume = getVolume;
		view->onSetVolume = setVolume;
	}
	if (onOpenSettings)
		view->onOpenSettings = [this]() { RunHidden(onOpenSettings); };
	if (onBackToLibrary)
		view->onBackToLibrary = [this]() { RunHidden(onBackToLibrary); };
	if (onQuit)
		view->onQuit = [this]() { RunHidden(onQuit); };

	m_panelFrame = panel;
	m_clip = clip;
	m_view = view;
}

// amount: 0 = hidden, 1 = fully open
// The windows keep a fixed size and only move (and change their opacity) while animating. Resizing a window on every frame
// made the system reallocate its surface and the panel repaint itself each time, which is what made the slide choppy.
void wxGameMenuOverlay::LayoutWindows(double amount)
{
	if (!m_panelFrame || !m_view)
		return;
	const wxSize client = m_owner->GetClientSize();
	const wxPoint origin = m_owner->ClientToScreen(wxPoint(0, 0));
	const wxRect rect(origin, client);
	m_lastRect = rect;

	const int panelW = PanelWidth(client);
	const bool geometryChanged = rect != m_laidOutRect || panelW != m_laidOutPanelW;
	if (geometryChanged)
	{
		if (m_dimFrame)
			m_dimFrame->SetSize(origin.x, origin.y, client.x, client.y);
		m_panelFrame->SetSize(origin.x + client.x - panelW, origin.y, panelW, client.y);
		m_view->SetSize(0, 0, panelW, client.y);
		m_laidOutRect = rect;
		m_laidOutPanelW = panelW;
		m_laidOutOffset = 0;
	}

	// slide: the panel window comes in from the right edge of the game window
	const int offset = (int)std::lround((1.0 - std::clamp(amount, 0.0, 1.0)) * panelW);
	if (geometryChanged || offset != m_laidOutOffset)
	{
		m_panelFrame->Move(origin.x + client.x - panelW + offset, origin.y);
		m_laidOutOffset = offset;
	}

	// the part that is still outside of the game window fades in with the slide
	if (m_panelLayered)
	{
		const int alpha = std::min(254, (int)std::lround(254.0 * std::min(1.0, amount * 1.6))); // 254, not 255: 255 would remove the layered style and cause a hitch
		if (alpha != m_laidOutPanelAlpha)
		{
			m_panelFrame->SetTransparent((wxByte)alpha);
			m_laidOutPanelAlpha = alpha;
		}
	}
	if (m_dimFrame)
	{
		const int alpha = (int)std::lround(kDimAlpha * amount);
		if (alpha != m_laidOutDimAlpha)
		{
			m_dimFrame->SetTransparent((wxByte)alpha);
			m_laidOutDimAlpha = alpha;
		}
	}
}

void wxGameMenuOverlay::Open()
{
	if (m_state != State::Hidden || m_paused || !onFetchMenu)
		return;

	// freeze the game first, the controller state can keep changing while we build the windows
	coreinit::OSSchedulerSetPaused(true);
	m_paused = true;
	m_pendingResume = false;

	EnsureWindows();
	m_amount = 0.0;
	LayoutWindows(0.0);
	if (m_dimFrame)
		m_dimFrame->ShowWithoutActivating();
	m_panelFrame->Show();
	m_panelFrame->Raise();
	if (isSoundEnabled)
		m_view->SetSoundEnabled(isSoundEnabled());
	m_view->OpenOverlayPanel();
	m_view->SetFocus();

	m_state = State::Opening;
	m_pollTimer.Start(kPollMs); // check the controller faster while the menu is open
	m_waitRelease = true; // the - + that opened the menu are still held
	m_prev = PadState();
	m_nextRepeatUp = m_nextRepeatDown = m_nextRepeatLeft = m_nextRepeatRight = 0;
	StartAnimation();
}

void wxGameMenuOverlay::Close()
{
	if (m_state == State::Hidden || m_state == State::Closing)
		return;
	m_state = State::Closing;
	RequestResume();
	StartAnimation();
}

void wxGameMenuOverlay::Animate()
{
	const long now = m_watch.Time();
	const double dt = std::min(0.05, (now - m_lastAnimMs) / 1000.0);
	m_lastAnimMs = now;

	const bool opening = m_state == State::Opening || m_state == State::Open;
	const double target = opening ? 1.0 : 0.0;
	m_amount += (target - m_amount) * (1.0 - std::exp(-dt * 14.0));
	if (std::fabs(target - m_amount) < 0.004)
		m_amount = target;
	LayoutWindows(m_amount);

	if (m_state == State::Opening && m_amount >= 1.0)
	{
		m_state = State::Open;
		m_animTimer.Stop();
	}
	else if (m_state == State::Closing && m_amount <= 0.0)
	{
		HideNow();
	}
}

void wxGameMenuOverlay::HideNow()
{
	m_animTimer.Stop();
	m_amount = 0.0;
	m_state = State::Hidden;
	m_pollTimer.Start(kPollIdleMs);
	if (m_panelFrame)
		m_panelFrame->Hide();
	if (m_dimFrame)
		m_dimFrame->Hide();
	if (onHidden)
		onHidden();
}

// for actions that open their own windows or dialogs: the menu disappears at once, the game stays paused while the action runs
void wxGameMenuOverlay::RunHidden(const std::function<void()>& action)
{
	HideNow();
	m_pendingResume = false;
	if (action)
		action();
	RequestResume();
}

void wxGameMenuOverlay::RequestResume()
{
	m_pendingResume = true;
	m_resumeRequestMs = m_watch.Time();
}

void wxGameMenuOverlay::ResumeNow()
{
	m_pendingResume = false;
	if (!m_paused)
		return;
	m_paused = false;
	coreinit::OSSchedulerSetPaused(false);
}
