#include "wxgui/components/wxGamepadNavigator.h"

#include <wx/app.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/combobox.h>
#include <wx/dataview.h>
#include <wx/dialog.h>
#include <wx/listbox.h>
#include <wx/listctrl.h>
#include <wx/notebook.h>
#include <wx/radiobox.h>
#include <wx/slider.h>
#include <wx/spinctrl.h>
#include <wx/textctrl.h>
#include <wx/toplevel.h>
#include <wx/treectrl.h>
#include <wx/propgrid/propgrid.h>

#include "wxgui/GameProfileWindow.h"
#include "wxgui/TitleManager.h"
#include "wxgui/input/InputSettings2.h"

#ifdef __WXMSW__
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <wx/msw/wrapwin.h>
#endif

#include <algorithm>

namespace
{
	constexpr int kTimerId = 1;
	constexpr long kPollMs = 33;
	constexpr long kRepeatDelayMs = 380;
	constexpr long kRepeatIntervalMs = 90;

	// controls where up / down / left / right already mean something (rows of a list, nodes of a tree...)
	bool IsListLike(wxWindow* w)
	{
		return wxDynamicCast(w, wxListBox) || wxDynamicCast(w, wxListCtrl) || wxDynamicCast(w, wxTreeCtrl) ||
			   wxDynamicCast(w, wxDataViewCtrl) || wxDynamicCast(w, wxPropertyGrid);
	}

	// presses and releases a real key: the focused native control reacts exactly as it does to the keyboard
	bool InjectKey(int virtualKey)
	{
#ifdef __WXMSW__
		INPUT input[2] = {};
		input[0].type = INPUT_KEYBOARD;
		input[0].ki.wVk = (WORD)virtualKey;
		if (virtualKey == VK_UP || virtualKey == VK_DOWN || virtualKey == VK_LEFT || virtualKey == VK_RIGHT)
			input[0].ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
		input[1] = input[0];
		input[1].ki.dwFlags |= KEYEVENTF_KEYUP;
		return ::SendInput(2, input, sizeof(INPUT)) == 2;
#else
		(void)virtualKey;
		return false;
#endif
	}

	enum { kKeyUp, kKeyDown, kKeyLeft, kKeyRight, kKeySpace, kKeyReturn };

	bool InjectNamed(int key)
	{
#ifdef __WXMSW__
		switch (key)
		{
		case kKeyUp: return InjectKey(VK_UP);
		case kKeyDown: return InjectKey(VK_DOWN);
		case kKeyLeft: return InjectKey(VK_LEFT);
		case kKeyRight: return InjectKey(VK_RIGHT);
		case kKeySpace: return InjectKey(VK_SPACE);
		default: return InjectKey(VK_RETURN);
		}
#else
		(void)key;
		return false;
#endif
	}

	void SendCommand(wxWindow* control, wxEventType type, int value = 0, const wxString& text = wxString())
	{
		wxCommandEvent event(type, control->GetId());
		event.SetEventObject(control);
		event.SetInt(value);
		event.SetString(text);
		control->GetEventHandler()->ProcessEvent(event);
	}

	// the control that owns the keyboard focus, even when the focus sits on one of its inner windows (edit part of a spin box...)
	wxWindow* ControlOf(wxWindow* focus, wxWindow* target)
	{
		for (wxWindow* w = focus; w && w != target; w = w->GetParent())
		{
			if (wxDynamicCast(w, wxComboBox) || wxDynamicCast(w, wxChoice) || wxDynamicCast(w, wxRadioBox) || wxDynamicCast(w, wxSlider) ||
				wxDynamicCast(w, wxSpinCtrl) || wxDynamicCast(w, wxSpinCtrlDouble) || wxDynamicCast(w, wxCheckBox) || wxDynamicCast(w, wxButton) ||
				IsListLike(w))
				return w;
		}
		return focus;
	}

	wxBookCtrlBase* FindBookIn(wxWindow* root)
	{
		for (wxWindow* child : root->GetChildren())
		{
			if (!child->IsShown())
				continue;
			if (auto* book = wxDynamicCast(child, wxBookCtrlBase))
				return book;
			if (auto* inner = FindBookIn(child))
				return inner;
		}
		return nullptr;
	}
}

wxGamepadNavigator::wxGamepadNavigator()
	: m_timer(this, kTimerId)
{
	Bind(wxEVT_TIMER, &wxGamepadNavigator::OnTimer, this);
	m_timer.Start(kPollMs);
}

void wxGamepadNavigator::Start()
{
	static wxGamepadNavigator* instance = new wxGamepadNavigator(); // lives as long as the application
	(void)instance;
}

void wxGamepadNavigator::OnTimer(wxTimerEvent&)
{
	Poll();
}

// the active window, if it is one the controller should drive
wxWindow* wxGamepadNavigator::FindTarget() const
{
	if (!wxTheApp || !wxTheApp->IsActive())
		return nullptr;
	wxWindow* active = wxGetActiveWindow();
	if (!active)
		return nullptr;
	wxWindow* top = wxGetTopLevelParent(active);
	if (!top || top == wxTheApp->GetTopWindow() || !top->IsShownOnScreen() || !top->IsEnabled())
		return nullptr;
	if (dynamic_cast<InputSettings2*>(top))
		return nullptr;
	if (dynamic_cast<wxDialog*>(top) || dynamic_cast<GameProfileWindow*>(top) || dynamic_cast<TitleManager*>(top))
		return top;
	return nullptr;
}

void wxGamepadNavigator::Poll()
{
	wxWindow* target = FindTarget();
	if (!target)
	{
		m_target = nullptr;
		m_waitRelease = true; // the next window starts waiting for released buttons
		return;
	}

	const wxGamepadState pad = wxReadGamepadState();
	if (target != m_target.get())
	{
		m_target = target;
		m_waitRelease = true;
		m_focusRectsShown = false;
		m_nextUp = m_nextDown = m_nextLeft = m_nextRight = 0;
	}
	if (m_waitRelease)
	{
		if (!pad.Any())
			m_waitRelease = false;
		m_prev = pad;
		return;
	}

	const long now = std::max<long>(1, m_watch.Time());
	auto repeat = [&](bool down, bool wasDown, long& next) {
		if (!down)
		{
			next = 0;
			return false;
		}
		if (!wasDown)
		{
			next = now + kRepeatDelayMs;
			return true;
		}
		if (now >= next)
		{
			next = now + kRepeatIntervalMs;
			return true;
		}
		return false;
	};
	const bool up = repeat(pad.up, m_prev.up, m_nextUp);
	const bool down = repeat(pad.down, m_prev.down, m_nextDown);
	const bool left = repeat(pad.left, m_prev.left, m_nextLeft);
	const bool right = repeat(pad.right, m_prev.right, m_nextRight);

	const wxGamepadState prev = m_prev;
	m_prev = pad;

	const bool any = up || down || left || right || (pad.accept && !prev.accept) || (pad.back && !prev.back) || (pad.favorite && !prev.favorite) ||
					 (pad.start && !prev.start) || (pad.tabPrev && !prev.tabPrev) || (pad.tabNext && !prev.tabNext);
	if (!any)
		return;

	// "back" first: Act() may destroy the window
	if (pad.back && !prev.back)
	{
		target->Close();
		return;
	}
	Act(target, wxGamepadState{pad.up && !prev.up, pad.down && !prev.down, pad.left && !prev.left, pad.right && !prev.right,
							   pad.accept && !prev.accept, false, pad.start && !prev.start, pad.favorite && !prev.favorite,
							   pad.tabPrev && !prev.tabPrev, pad.tabNext && !prev.tabNext},
		up, down, left, right);
}

// edge = what was just pressed (one shot), up / down / left / right already include the auto repeat
void wxGamepadNavigator::Act(wxWindow* target, const wxGamepadState& edge, bool up, bool down, bool left, bool right)
{
	ShowFocusRects(target);

	wxWindow* focus = wxWindow::FindFocus();
	if (!focus || wxGetTopLevelParent(focus) != target)
	{
		// nothing focused in this window yet: the first press only lands the focus on the first control
		target->Navigate(wxNavigationKeyEvent::IsForward);
		return;
	}
	wxWindow* control = ControlOf(focus, target);
	const bool listLike = IsListLike(control);

	if (edge.tabNext || edge.tabPrev)
	{
		ChangeTab(target, focus, edge.tabNext);
		return;
	}
	if (edge.favorite || edge.start)
	{
		MoveFocus(target, edge.favorite); // Y forward, + backward: the way out of a list
		return;
	}
	if (edge.accept)
	{
		Accept(control);
		return;
	}

	if (up || down)
	{
		if (listLike && InjectNamed(up ? kKeyUp : kKeyDown))
			return;
		MoveFocus(target, down);
		return;
	}
	if (left || right)
	{
		if (listLike)
			InjectNamed(left ? kKeyLeft : kKeyRight);
		else
			ChangeValue(control, left ? -1 : 1);
	}
}

void wxGamepadNavigator::MoveFocus(wxWindow* target, bool forward)
{
	wxWindow* focus = wxWindow::FindFocus();
	wxWindow* from = (focus && wxGetTopLevelParent(focus) == target) ? focus : target;
	from->Navigate(forward ? wxNavigationKeyEvent::IsForward : wxNavigationKeyEvent::IsBackward);
}

void wxGamepadNavigator::ChangeTab(wxWindow* target, wxWindow* focus, bool forward)
{
	wxBookCtrlBase* book = nullptr;
	for (wxWindow* w = focus; w; w = w->GetParent())
	{
		book = wxDynamicCast(w, wxBookCtrlBase);
		if (book)
			break;
	}
	if (!book)
		book = FindBookIn(target);
	if (!book || book->GetPageCount() < 2)
	{
		MoveFocus(target, forward); // no tabs here: L / R move between controls
		return;
	}
	const int count = (int)book->GetPageCount();
	const int next = (book->GetSelection() + count + (forward ? 1 : -1)) % count;
	book->SetSelection(next);
	if (wxWindow* page = book->GetCurrentPage())
		page->SetFocus();
}

void wxGamepadNavigator::ChangeValue(wxWindow* control, int direction)
{
	if (auto* radio = wxDynamicCast(control, wxRadioBox))
	{
		const int count = (int)radio->GetCount();
		if (count < 2)
			return;
		const int next = (radio->GetSelection() + count + direction) % count;
		radio->SetSelection(next);
		SendCommand(radio, wxEVT_RADIOBOX, next, radio->GetString(next));
		return;
	}
	if (auto* combo = wxDynamicCast(control, wxComboBox))
	{
		const int count = (int)combo->GetCount();
		if (count < 1)
			return;
		const int current = combo->GetSelection();
		const int next = current == wxNOT_FOUND ? 0 : std::clamp(current + direction, 0, count - 1);
		if (next == current)
			return;
		combo->SetSelection(next);
		SendCommand(combo, wxEVT_COMBOBOX, next, combo->GetString(next));
		return;
	}
	if (auto* choice = wxDynamicCast(control, wxChoice))
	{
		const int count = (int)choice->GetCount();
		if (count < 1)
			return;
		const int current = choice->GetSelection();
		const int next = current == wxNOT_FOUND ? 0 : std::clamp(current + direction, 0, count - 1);
		if (next == current)
			return;
		choice->SetSelection(next);
		SendCommand(choice, wxEVT_CHOICE, next, choice->GetString(next));
		return;
	}
	if (auto* slider = wxDynamicCast(control, wxSlider))
	{
		const int range = slider->GetMax() - slider->GetMin();
		const int step = std::max({1, slider->GetLineSize(), range / 20});
		const int value = std::clamp(slider->GetValue() + direction * step, slider->GetMin(), slider->GetMax());
		if (value == slider->GetValue())
			return;
		slider->SetValue(value);
		wxScrollEvent scroll(wxEVT_SCROLL_CHANGED, slider->GetId(), value, wxHORIZONTAL);
		scroll.SetEventObject(slider);
		slider->GetEventHandler()->ProcessEvent(scroll);
		SendCommand(slider, wxEVT_SLIDER, value);
		return;
	}
	if (auto* spin = wxDynamicCast(control, wxSpinCtrl))
	{
		const int value = std::clamp(spin->GetValue() + direction, spin->GetMin(), spin->GetMax());
		if (value == spin->GetValue())
			return;
		spin->SetValue(value);
		wxSpinEvent event(wxEVT_SPINCTRL, spin->GetId());
		event.SetEventObject(spin);
		event.SetInt(value);
		spin->GetEventHandler()->ProcessEvent(event);
		return;
	}
	if (auto* spin = wxDynamicCast(control, wxSpinCtrlDouble))
	{
		const double value = std::clamp(spin->GetValue() + direction * spin->GetIncrement(), spin->GetMin(), spin->GetMax());
		if (value == spin->GetValue())
			return;
		spin->SetValue(value);
		wxSpinDoubleEvent event(wxEVT_SPINCTRLDOUBLE, spin->GetId(), value);
		event.SetEventObject(spin);
		spin->GetEventHandler()->ProcessEvent(event);
		return;
	}
}

void wxGamepadNavigator::Accept(wxWindow* control)
{
	if (auto* check = wxDynamicCast(control, wxCheckBox))
	{
		if (check->Is3State())
			return;
		check->SetValue(!check->GetValue());
		SendCommand(check, wxEVT_CHECKBOX, check->GetValue() ? 1 : 0);
		return;
	}
	if (auto* button = wxDynamicCast(control, wxButton))
	{
		// may close the window: nothing touches the window afterwards
		SendCommand(button, wxEVT_BUTTON);
		return;
	}
	if (wxDynamicCast(control, wxRadioBox) || wxDynamicCast(control, wxChoice) || wxDynamicCast(control, wxComboBox))
	{
		// choices are never dropped open (the list would swallow the controller): accept goes to the next value, wrapping around
		int count = 0, current = wxNOT_FOUND;
		if (auto* radio = wxDynamicCast(control, wxRadioBox))
			count = (int)radio->GetCount(), current = radio->GetSelection();
		else if (auto* combo = wxDynamicCast(control, wxComboBox))
			count = (int)combo->GetCount(), current = combo->GetSelection();
		else if (auto* choice = wxDynamicCast(control, wxChoice))
			count = (int)choice->GetCount(), current = choice->GetSelection();
		if (count < 2)
			return;
		if (current >= count - 1)
		{
			// wrap to the first value: ChangeValue clamps, so set it here
			if (auto* radio = wxDynamicCast(control, wxRadioBox))
			{
				radio->SetSelection(0);
				SendCommand(radio, wxEVT_RADIOBOX, 0, radio->GetString(0));
			}
			else if (auto* combo = wxDynamicCast(control, wxComboBox))
			{
				combo->SetSelection(0);
				SendCommand(combo, wxEVT_COMBOBOX, 0, combo->GetString(0));
			}
			else if (auto* choice = wxDynamicCast(control, wxChoice))
			{
				choice->SetSelection(0);
				SendCommand(choice, wxEVT_CHOICE, 0, choice->GetString(0));
			}
		}
		else
			ChangeValue(control, 1);
		return;
	}
	if (IsListLike(control))
	{
		// trees with check boxes (graphic packs) toggle with space, everything else activates with enter
		InjectNamed(wxDynamicCast(control, wxTreeCtrl) ? kKeySpace : kKeyReturn);
		return;
	}
}

// Windows hides the focus rectangle until a key is used; with the controller nobody pressed a key, so show it
void wxGamepadNavigator::ShowFocusRects(wxWindow* target)
{
#ifdef __WXMSW__
	if (m_focusRectsShown)
		return;
	m_focusRectsShown = true;
	::SendMessageW((HWND)target->GetHandle(), WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS | UISF_HIDEACCEL), 0);
#else
	(void)target;
#endif
}
