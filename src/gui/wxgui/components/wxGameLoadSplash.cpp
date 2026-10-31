#include "wxgui/components/wxGameLoadSplash.h"

#include <wx/dcbuffer.h>
#include <wx/graphics.h>

#ifdef __WXMSW__
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <wx/msw/wrapwin.h>
#endif

#include <algorithm>
#include <cmath>
#include <memory>

// hooks in LatteShaderCache.cpp
bool LatteShaderCache_GameScreenIsReady();
void LatteShaderCache_ResetGameScreenReady();
bool LatteShaderCache_GameIsRendering(); // the game itself draws frames (not only Cemu's shader loading screen)

namespace
{
	constexpr int kTimerId = 1;
	constexpr long kFadeInMs = 250;
	constexpr long kFadeOutMs = 250;
	constexpr long kMinShownMs = 500;      // avoids a flash when Cemu's own screen appears right away
	constexpr long kSafetyTimeoutMs = 60000; // never stay on screen longer than this

	// tip about the in-game menu
	constexpr long kHintFadeInMs = 250;
	constexpr long kHintShownMs = 5000;   // time on screen once the game is running
	constexpr long kHintFadeOutMs = 500;
	constexpr long kHintMaxWaitMs = 90000; // if the game never reports frames, show the tip anyway after this long
}

wxGameLoadSplash::wxGameLoadSplash(wxFrame* owner, const wxString& name, const wxBitmap& icon, const std::shared_ptr<const wxImage>& art)
	: wxFrame(owner, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(10, 10), wxFRAME_NO_TASKBAR | wxFRAME_FLOAT_ON_PARENT | wxBORDER_NONE),
	  m_owner(owner), m_name(name), m_icon(icon), m_timer(this, kTimerId)
{
	SetBackgroundStyle(wxBG_STYLE_PAINT);
	SetBackgroundColour(wxColour(8, 10, 18));
	if (art && art->IsOk())
		m_art = wxBitmap(*art);

	LatteShaderCache_ResetGameScreenReady();

	m_transparentOk = CanSetTransparent();
	Reposition();
	SetAlpha(0);
	ShowWithoutActivating();

	Bind(wxEVT_PAINT, &wxGameLoadSplash::OnPaint, this);
	Bind(wxEVT_TIMER, &wxGameLoadSplash::OnTimer, this);
	Bind(wxEVT_LEFT_DOWN, &wxGameLoadSplash::OnClick, this);
	m_timer.Start(33);
}

wxGameLoadSplash::~wxGameLoadSplash()
{
	m_timer.Stop();
	// same reason as in wxGameTileView: late events must not reach a half destroyed object
	Unbind(wxEVT_PAINT, &wxGameLoadSplash::OnPaint, this);
	Unbind(wxEVT_TIMER, &wxGameLoadSplash::OnTimer, this);
	Unbind(wxEVT_LEFT_DOWN, &wxGameLoadSplash::OnClick, this);
}

void wxGameLoadSplash::Dismiss()
{
	if (m_closing)
		return;
	m_closing = true;
	m_closeStartMs = m_watch.Time();
}

void wxGameLoadSplash::SetAlpha(int alpha)
{
	alpha = std::clamp(alpha, 0, 254); // 255 would remove the layered style
	if (alpha == m_alpha)
		return;
	m_alpha = alpha;
	if (m_transparentOk)
		SetTransparent((wxByte)alpha);
}

// the splash covers the client area of the main window and follows it (fullscreen switches while loading)
void wxGameLoadSplash::Reposition()
{
	if (!m_owner)
		return;
	const wxRect rect(m_owner->ClientToScreen(wxPoint(0, 0)), m_owner->GetClientSize());
	if (rect == m_lastRect)
		return;
	m_lastRect = rect;
	SetSize(rect);
	Refresh(false);
}

void wxGameLoadSplash::OnClick(wxMouseEvent&)
{
	m_showHint = true;
	Dismiss();
}

void wxGameLoadSplash::OnTimer(wxTimerEvent&)
{
	const long now = m_watch.Time();
	Reposition();

	if (!m_closing)
	{
		SetAlpha((int)std::lround(254.0 * std::min(1.0, now / (double)kFadeInMs)));
		if ((now >= kMinShownMs && LatteShaderCache_GameScreenIsReady()) || now >= kSafetyTimeoutMs)
		{
			m_showHint = true;
			Dismiss();
		}
	}
	else
	{
		const double t = (m_watch.Time() - m_closeStartMs) / (double)kFadeOutMs;
		if (t >= 1.0)
		{
			m_timer.Stop();
			Hide();
			if (m_showHint && m_owner)
				new wxGameHintToast(m_owner); // owned by the main window, closes itself
			Destroy();
			return;
		}
		SetAlpha((int)std::lround(254.0 * (1.0 - t)));
	}

	// the dots of "Cargando..." move: repaint a few times per second
	if ((now / 33) % 4 == 0)
		Refresh(false);
}

void wxGameLoadSplash::OnPaint(wxPaintEvent&)
{
	wxAutoBufferedPaintDC dc(this);
	std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
	if (!gc)
		return;
	const wxSize size = GetClientSize();
	const double W = size.x;
	const double H = size.y;
	if (W <= 0 || H <= 0)
		return;
	const double dpi = GetDPIScaleFactor();

	gc->SetInterpolationQuality(wxINTERPOLATION_BEST);
	gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
	gc->SetPen(*wxTRANSPARENT_PEN);
	gc->SetBrush(gc->CreateLinearGradientBrush(0, 0, 0, H, wxColour(14, 19, 38), wxColour(4, 5, 11)));
	gc->DrawRectangle(0, 0, W, H);

	// game art, "cover" fit like the library background
	if (m_art.IsOk())
	{
		const double scale = std::max(W / m_art.GetWidth(), H / m_art.GetHeight());
		const double dw = m_art.GetWidth() * scale;
		const double dh = m_art.GetHeight() * scale;
		gc->DrawBitmap(m_art, (W - dw) / 2, (H - dh) / 2, dw, dh);
		gc->SetBrush(wxBrush(wxColour(0, 0, 0, 55)));
		gc->DrawRectangle(0, 0, W, H);
	}
	gc->SetBrush(gc->CreateLinearGradientBrush(0, H * 0.55, 0, H, wxColour(0, 0, 0, 0), wxColour(0, 0, 0, 215)));
	gc->DrawRectangle(0, 0, W, H);

	// name and "Cargando..." at the bottom left (same anchor as the library)
	const double anchorX = std::max(56.0 * dpi, W * 0.08);
	const double iconSize = 72.0 * dpi;
	const double bottom = H - 64.0 * dpi;
	double textX = anchorX;
	if (m_icon.IsOk())
	{
		gc->DrawBitmap(m_icon, anchorX, bottom - iconSize, iconSize, iconSize);
		textX = anchorX + iconSize + 20.0 * dpi;
	}
	const wxFont titleFont = GetFont().Scaled(2.0).Bold();
	const wxFont subFont = GetFont().Scaled(1.25);
	wxDouble tw, th, sw, sh;
	gc->SetFont(titleFont, wxColour(255, 255, 255));
	gc->GetTextExtent(wxString::FromUTF8("Ag"), &tw, &th);
	const int dots = (int)((m_watch.Time() / 400) % 4);
	const wxString sub = wxString::FromUTF8("Cargando") + wxString(wxUniChar('.'), dots);
	gc->SetFont(subFont, wxColour(170, 178, 200));
	gc->GetTextExtent(wxString::FromUTF8("Cargando..."), &sw, &sh);
	const double block = th + 6.0 * dpi + sh;
	const double top = bottom - iconSize / 2 - block / 2;
	gc->SetFont(titleFont, wxColour(255, 255, 255));
	gc->DrawText(m_name, textX, top);
	gc->SetFont(subFont, wxColour(170, 178, 200));
	gc->DrawText(sub, textX, top + th + 6.0 * dpi);

	// tip: how to open the in-game menu
	gc->SetFont(GetFont().Scaled(1.1), wxColour(170, 178, 200, 190));
	gc->DrawText(wxString::FromUTF8("Dentro del juego: mant\xC3\xA9n  -  y  +  (o F1) para abrir el men\xC3\xBA"), anchorX, bottom + 20.0 * dpi);
}

// ---------------------------------------------------------------------------------------------------------------------

wxGameHintToast::wxGameHintToast(wxFrame* owner)
	: wxFrame(owner, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(10, 10), wxFRAME_NO_TASKBAR | wxFRAME_FLOAT_ON_PARENT | wxBORDER_NONE),
	  m_owner(owner), m_timer(this, kTimerId)
{
	SetBackgroundStyle(wxBG_STYLE_PAINT);
	SetBackgroundColour(wxColour(10, 13, 24));
	m_text = wxString::FromUTF8("Dentro del juego: mant\xC3\xA9n  -  y  +  (o F1) para abrir el men\xC3\xBA");

	const double dpi = GetDPIScaleFactor();
	const wxFont font = GetFont().Scaled(1.2);
	int tw = 0, th = 0;
	GetTextExtent(m_text, &tw, &th, nullptr, nullptr, &font);
	m_boxSize = wxSize(tw + (int)(56.0 * dpi), th + (int)(28.0 * dpi));

	m_transparentOk = CanSetTransparent();
	SetAlpha(0);
#ifdef __WXMSW__
	// clicks go through to the game and the tip never takes the keyboard focus away from it
	const HWND hwnd = (HWND)GetHandle();
	::SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE) | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
#endif

	Bind(wxEVT_PAINT, &wxGameHintToast::OnPaint, this);
	Bind(wxEVT_TIMER, &wxGameHintToast::OnTimer, this);
	m_timer.Start(33); // the window stays hidden until the game is running
}

wxGameHintToast::~wxGameHintToast()
{
	m_timer.Stop();
	// same reason as in wxGameLoadSplash: late events must not reach a half destroyed object
	Unbind(wxEVT_PAINT, &wxGameHintToast::OnPaint, this);
	Unbind(wxEVT_TIMER, &wxGameHintToast::OnTimer, this);
}

void wxGameHintToast::SetAlpha(int alpha)
{
	alpha = std::clamp(alpha, 0, 254); // 255 would remove the layered style
	if (alpha == m_alpha)
		return;
	m_alpha = alpha;
	if (m_transparentOk)
		SetTransparent((wxByte)alpha);
}

// bottom center of the client area of the main window, it follows the window (fullscreen switches)
void wxGameHintToast::Reposition()
{
	if (!m_owner)
		return;
	const wxPoint origin = m_owner->ClientToScreen(wxPoint(0, 0));
	const wxSize client = m_owner->GetClientSize();
	const double dpi = GetDPIScaleFactor();
	const wxRect rect(origin.x + (client.x - m_boxSize.x) / 2, origin.y + client.y - m_boxSize.y - (int)(72.0 * dpi), m_boxSize.x, m_boxSize.y);
	if (rect == m_lastRect)
		return;
	m_lastRect = rect;
	SetSize(rect);
	Refresh(false);
}

void wxGameHintToast::OnTimer(wxTimerEvent&)
{
	const long now = m_watch.Time();
	if (m_startMs < 0)
	{
		if (!LatteShaderCache_GameIsRendering() && now < kHintMaxWaitMs)
			return;
		m_startMs = std::max<long>(1, now);
		Reposition();
		ShowWithoutActivating();
	}
	Reposition();

	const long t = now - m_startMs;
	const long fadeOutStart = kHintFadeInMs + kHintShownMs;
	if (t >= fadeOutStart + kHintFadeOutMs)
	{
		m_timer.Stop();
		Hide();
		Destroy();
		return;
	}
	double a = 1.0;
	if (t < kHintFadeInMs)
		a = t / (double)kHintFadeInMs;
	else if (t > fadeOutStart)
		a = 1.0 - (t - fadeOutStart) / (double)kHintFadeOutMs;
	SetAlpha((int)std::lround(235.0 * std::clamp(a, 0.0, 1.0)));
}

void wxGameHintToast::OnPaint(wxPaintEvent&)
{
	wxAutoBufferedPaintDC dc(this);
	std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
	if (!gc)
		return;
	const wxSize size = GetClientSize();
	const double dpi = GetDPIScaleFactor();
	gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
	gc->SetPen(wxPen(wxColour(255, 255, 255, 70), 1));
	gc->SetBrush(wxBrush(wxColour(10, 13, 24)));
	gc->DrawRoundedRectangle(0.5, 0.5, size.x - 1.0, size.y - 1.0, 10.0 * dpi);

	gc->SetFont(GetFont().Scaled(1.2), wxColour(235, 240, 255));
	wxDouble tw, th;
	gc->GetTextExtent(m_text, &tw, &th);
	gc->DrawText(m_text, (size.x - tw) / 2.0, (size.y - th) / 2.0);
}
