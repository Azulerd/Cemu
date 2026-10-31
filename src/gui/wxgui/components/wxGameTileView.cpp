#include "wxgui/components/wxGameTileView.h"

#include <wx/dcbuffer.h>
#include <wx/dcclient.h>
#include <wx/graphics.h>
#include <wx/geometry.h>
#include <wx/datetime.h>
#include <wx/control.h>
#include <wx/intl.h>
#include <wx/settings.h>
#include <wx/sound.h>
#include <wx/app.h>
#include <wx/dcmemory.h>
#include <wx/utils.h>
#include <wx/clipbrd.h>
#ifdef __WXMSW__
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <wx/msw/wrapwin.h>
#include <mmsystem.h>
#endif

#include <algorithm>
#include <cstring>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <cstdint>
#include <vector>
#include <memory>
#include <vector>

namespace
{
	constexpr double kSelectedScale = 1.32; // size of the focused tile relative to the others
	constexpr int kTileTexSize = 256;       // resolution of the pre-rendered rounded tiles
	constexpr double kCornerRadius = 0.14;  // relative to tile size
	constexpr int kAmbientW = 48;           // resolution of the blurred background source
	constexpr int kAmbientH = 27;
	constexpr int kBgW = 640;               // resolution of the blended background (matches wxGameList::kArtWidth/kArtHeight)
	constexpr int kBgH = 360;
	constexpr int kAnimTimerId = 1;
	constexpr int kPollTimerId = 2;
	constexpr int kPadTimerId = 3;
	constexpr long kPadPollMs = 33;
	constexpr long kPadRepeatDelayMs = 380;
	constexpr long kPadRepeatIntervalMs = 90;
	constexpr int kTabCount = 4; // Juegos, Recientes, Favoritos, Más jugados (new tabs go last: the saved tab number stays valid)
	constexpr size_t kMaxRecent = 15;
	constexpr size_t kMaxMostPlayed = 15;
	constexpr int kSortModeCount = 3;
	constexpr double kLaunchFadeMs = 420.0;   // menu fades to black over this time before the game starts
	constexpr long kLaunchTimeoutMs = 700;    // if the view is still alive this long after requesting the launch, it failed -> fade back in
	constexpr double kLaunchFadeInMs = 300.0; // fade back in after a failed launch

	double Clamp01(double v)
	{
		return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
	}

	double Lerp(double a, double b, double t)
	{
		return a + (b - a) * t;
	}

	wxString TabName(int index)
	{
		switch (index)
		{
		case 0: return wxString::FromUTF8("Juegos");
		case 1: return wxString::FromUTF8("Recientes");
		case 2: return wxString::FromUTF8("Favoritos");
		default: return wxString::FromUTF8("Más jugados");
		}
	}

	// heart outline centered on (cx, cy); s is roughly half of its width
	wxGraphicsPath MakeHeartPath(wxGraphicsContext& gc, double cx, double cy, double s)
	{
		wxGraphicsPath p = gc.CreatePath();
		p.MoveToPoint(cx, cy + s * 0.90);
		p.AddCurveToPoint(cx - s * 1.45, cy - s * 0.05, cx - s * 0.95, cy - s * 1.15, cx, cy - s * 0.35);
		p.AddCurveToPoint(cx + s * 0.95, cy - s * 1.15, cx + s * 1.45, cy - s * 0.05, cx, cy + s * 0.90);
		p.CloseSubpath();
		return p;
	}

	double SmoothStep(double t)
	{
		t = Clamp01(t);
		return t * t * (3.0 - 2.0 * t);
	}

	// builds a 16-bit mono 44.1 kHz WAV in memory from a synth function (time in seconds -> sample in [-1, 1])
	template<typename F>
	std::vector<unsigned char> MakeWav(double seconds, F synth)
	{
		const int rate = 44100;
		const int n = (int)(seconds * rate);
		std::vector<unsigned char> d(44 + n * 2);
		auto w32 = [&](int off, uint32_t v) { for (int i = 0; i < 4; ++i) d[off + i] = (unsigned char)((v >> (8 * i)) & 0xFF); };
		auto w16 = [&](int off, uint16_t v) { d[off] = (unsigned char)(v & 0xFF); d[off + 1] = (unsigned char)(v >> 8); };
		memcpy(&d[0], "RIFF", 4);
		w32(4, 36 + n * 2);
		memcpy(&d[8], "WAVE", 4);
		memcpy(&d[12], "fmt ", 4);
		w32(16, 16);
		w16(20, 1); // PCM
		w16(22, 1); // mono
		w32(24, rate);
		w32(28, rate * 2);
		w16(32, 2);
		w16(34, 16);
		memcpy(&d[36], "data", 4);
		w32(40, n * 2);
		for (int i = 0; i < n; ++i)
		{
			const double t = i / (double)rate;
			const double tail = std::min(1.0, (seconds - t) / 0.02); // short taper at the very end, avoids clicks
			double v = synth(t) * tail;
			v = std::max(-1.0, std::min(1.0, v));
			w16(44 + i * 2, (uint16_t)(int16_t)std::lround(v * 32767.0));
		}
		return d;
	}

#ifdef __WXMSW__
	// Plays the menu sounds from a worker thread. PlaySound opens the audio device on every call and can block for tens of
	// milliseconds (or longer when the device is busy), which made the whole interface stutter when it ran on the UI thread.
	class MenuSoundWorker
	{
	public:
		static MenuSoundWorker& Instance()
		{
			static MenuSoundWorker* instance = new MenuSoundWorker(); // never destroyed on purpose, the thread simply ends with the process
			return *instance;
		}

		void Play(const std::shared_ptr<const std::vector<unsigned char>>& wav)
		{
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				m_pending = wav; // only the newest request matters
			}
			m_cv.notify_one();
		}

	private:
		MenuSoundWorker()
		{
			std::thread([this]() { Run(); }).detach();
		}

		void Run()
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			while (true)
			{
				m_cv.wait(lock, [this]() { return m_pending != nullptr; });
				const std::shared_ptr<const std::vector<unsigned char>> wav = std::move(m_pending);
				m_pending.reset();
				lock.unlock();
				::PlaySoundW(reinterpret_cast<LPCWSTR>(wav->data()), nullptr, SND_MEMORY | SND_SYNC | SND_NODEFAULT);
				lock.lock();
			}
		}

		std::mutex m_mutex;
		std::condition_variable m_cv;
		std::shared_ptr<const std::vector<unsigned char>> m_pending;
	};
#endif

	wxColour WithAlpha(const wxColour& c, double alpha)
	{
		return wxColour(c.Red(), c.Green(), c.Blue(), (unsigned char)std::lround(Clamp01(alpha) * 255.0));
	}

	// word-wraps text into at most maxLines lines that fit maxWidth (measured with the font currently set on dc);
	// whatever does not fit in the last line is cut with an ellipsis
	std::vector<wxString> WrapText(const wxString& text, wxDC& dc, int maxWidth, size_t maxLines)
	{
		std::vector<wxString> words;
		size_t pos = 0;
		while (pos < text.length())
		{
			size_t end = text.find(' ', pos);
			if (end == wxString::npos)
				end = text.length();
			if (end > pos)
				words.push_back(text.Mid(pos, end - pos));
			pos = end + 1;
		}

		std::vector<wxString> lines;
		wxString current;
		for (const wxString& word : words)
		{
			wxString candidate = current;
			if (!candidate.empty())
				candidate += " ";
			candidate += word;
			if (lines.size() + 1 >= maxLines || current.empty() || dc.GetTextExtent(candidate).x <= maxWidth)
				current = candidate; // the last allowed line takes every remaining word, the ellipsis below cuts what does not fit
			else
			{
				lines.push_back(current);
				current = word;
			}
		}
		lines.push_back(current);

		for (wxString& line : lines)
			line = wxControl::Ellipsize(line, dc, wxELLIPSIZE_END, maxWidth);
		return lines;
	}

	// splits a message into short lines for the panel (by words, at most maxChars characters per line)
	std::vector<wxString> WrapByChars(const wxString& text, size_t maxChars)
	{
		std::vector<wxString> lines;
		wxString current;
		size_t pos = 0;
		while (pos < text.length())
		{
			size_t end = text.find(' ', pos);
			if (end == wxString::npos)
				end = text.length();
			const wxString word = text.Mid(pos, end - pos);
			pos = end + 1;
			if (word.empty())
				continue;
			if (!current.empty() && current.length() + 1 + word.length() > maxChars)
			{
				lines.push_back(current);
				current.clear();
			}
			if (!current.empty())
				current += " ";
			current += word;
		}
		if (!current.empty())
			lines.push_back(current);
		return lines;
	}

	// square image with anti-aliased rounded corners (alpha channel)
	wxBitmap MakeRoundedTile(const wxImage& src, int size, double radius)
	{
		wxImage img = src.Scale(size, size, wxIMAGE_QUALITY_BICUBIC);
		if (!img.IsOk())
			return wxBitmap();
		if (!img.HasAlpha())
			img.InitAlpha();
		unsigned char* alpha = img.GetAlpha();
		const double half = size * 0.5;
		for (int y = 0; y < size; ++y)
		{
			for (int x = 0; x < size; ++x)
			{
				const double px = std::fabs(x + 0.5 - half) - (half - radius);
				const double py = std::fabs(y + 0.5 - half) - (half - radius);
				const double d = std::hypot(std::max(px, 0.0), std::max(py, 0.0)) + std::min(std::max(px, py), 0.0) - radius;
				const double coverage = Clamp01(0.5 - d);
				unsigned char& a = alpha[y * size + x];
				a = (unsigned char)std::lround(a * coverage);
			}
		}
		return wxBitmap(img, 32);
	}

	// average colour weighted towards vivid pixels, brightened so it works as a glow
	wxColour DominantColour(const wxImage& img)
	{
		const unsigned char* data = img.GetData();
		const int w = img.GetWidth();
		const int h = img.GetHeight();
		double r = 0, g = 0, b = 0, sum = 0;
		if (data)
		{
			for (int y = 0; y < h; y += 3)
			{
				for (int x = 0; x < w; x += 3)
				{
					const unsigned char* p = data + (y * w + x) * 3;
					const int mx = std::max({p[0], p[1], p[2]});
					const int mn = std::min({p[0], p[1], p[2]});
					const double sat = mx ? (mx - mn) / (double)mx : 0.0;
					const double weight = 0.05 + sat * sat * (mx / 255.0);
					r += p[0] * weight;
					g += p[1] * weight;
					b += p[2] * weight;
					sum += weight;
				}
			}
		}
		if (sum <= 0.0)
			return wxColour(60, 120, 230);
		r /= sum;
		g /= sum;
		b /= sum;
		const double mx = std::max({r, g, b, 1.0});
		const double k = std::min(3.0, std::max(1.0, 185.0 / mx));
		return wxColour((unsigned char)std::min(255.0, r * k), (unsigned char)std::min(255.0, g * k), (unsigned char)std::min(255.0, b * k));
	}
}

namespace
{
	struct PadLabels
	{
		const char* accept;   // button at the bottom (accept)
		const char* back;     // button at the right (back)
		const char* favorite; // button at the left (favorite)
		const char* tabs;     // shoulder buttons
	};

	PadLabels GetPadLabels(int style)
	{
		switch (style)
		{
		case 1: return {"A", "B", "X", "LB RB"};              // Xbox
		case 2: return {"B", "A", "Y", "L R"};                // Nintendo (bottom button is B)
		default: return {"X", "O", "Cuadrado", "L1 R1"};      // PlayStation
		}
	}

	const char* SortModeName(int mode)
	{
		switch (mode)
		{
		case 1: return "Tiempo jugado";
		case 2: return "Última partida";
		default: return "Nombre";
		}
	}

	const char* TileSizeName(int size)
	{
		switch (size)
		{
		case 0: return "Pequeño";
		case 2: return "Grande";
		default: return "Normal";
		}
	}

	const char* AccentName(int accent)
	{
		switch (accent)
		{
		case 1: return "Azul";
		case 2: return "Violeta";
		case 3: return "Rosa";
		case 4: return "Verde";
		case 5: return "Naranja";
		case 6: return "Blanco";
		default: return "Automático";
		}
	}

	const char* ButtonStyleName(int setting)
	{
		switch (setting)
		{
		case 1: return "PlayStation";
		case 2: return "Xbox";
		case 3: return "Nintendo";
		default: return "Automático";
		}
	}
}

wxGameTileView::wxGameTileView(wxWindow* parent, wxWindowID id)
	: wxWindow(parent, id, wxDefaultPosition, wxDefaultSize, wxWANTS_CHARS | wxFULL_REPAINT_ON_RESIZE),
	  m_animTimer(this, kAnimTimerId), m_pollTimer(this, kPollTimerId), m_padTimer(this, kPadTimerId)
{
	SetBackgroundStyle(wxBG_STYLE_PAINT);
	SetBackgroundColour(wxColour(8, 10, 18));
	SetCanFocus(true);

	Bind(wxEVT_PAINT, &wxGameTileView::OnPaint, this);
	Bind(wxEVT_SIZE, &wxGameTileView::OnSize, this);
	Bind(wxEVT_KEY_DOWN, &wxGameTileView::OnKeyDown, this);
	Bind(wxEVT_CHAR, &wxGameTileView::OnChar, this);
	Bind(wxEVT_LEFT_DOWN, &wxGameTileView::OnMouseDown, this);
	Bind(wxEVT_LEFT_DCLICK, &wxGameTileView::OnMouseDClick, this);
	Bind(wxEVT_RIGHT_UP, &wxGameTileView::OnMouseRightUp, this);
	Bind(wxEVT_MOUSEWHEEL, &wxGameTileView::OnMouseWheel, this);
	Bind(wxEVT_MOTION, &wxGameTileView::OnMouseMove, this);
	Bind(wxEVT_SHOW, &wxGameTileView::OnShow, this);
	Bind(wxEVT_TIMER, &wxGameTileView::OnTimer, this);

	BuildSounds();
}

wxGameTileView::~wxGameTileView()
{
	SaveState(true);
	m_animTimer.Stop();
	m_pollTimer.Stop();
	m_padTimer.Stop();

	// While the base class window is destroyed after this destructor, Windows still sends it events (hide, size...) and wx
	// would call the handlers below on an object whose members (timers, callbacks) are already gone: that was the access
	// violation at exit. Unbinding them here makes those late events harmless.
	Unbind(wxEVT_PAINT, &wxGameTileView::OnPaint, this);
	Unbind(wxEVT_SIZE, &wxGameTileView::OnSize, this);
	Unbind(wxEVT_KEY_DOWN, &wxGameTileView::OnKeyDown, this);
	Unbind(wxEVT_CHAR, &wxGameTileView::OnChar, this);
	Unbind(wxEVT_LEFT_DOWN, &wxGameTileView::OnMouseDown, this);
	Unbind(wxEVT_LEFT_DCLICK, &wxGameTileView::OnMouseDClick, this);
	Unbind(wxEVT_RIGHT_UP, &wxGameTileView::OnMouseRightUp, this);
	Unbind(wxEVT_MOUSEWHEEL, &wxGameTileView::OnMouseWheel, this);
	Unbind(wxEVT_MOTION, &wxGameTileView::OnMouseMove, this);
	Unbind(wxEVT_SHOW, &wxGameTileView::OnShow, this);
	Unbind(wxEVT_TIMER, &wxGameTileView::OnTimer, this);
}

void wxGameTileView::BuildSounds()
{
	const double twoPi = 6.283185307179586;
	// soft short "tick" when moving between games
	m_tickData = std::make_shared<const std::vector<unsigned char>>(MakeWav(0.09, [&](double t) {
		const double env = std::min(1.0, t / 0.0015) * std::exp(-t * 55.0);
		return 0.30 * env * (0.8 * std::sin(twoPi * 1180.0 * t) + 0.2 * std::sin(twoPi * 2360.0 * t));
	}));
	// two-note chime when a game is started
	m_launchData = std::make_shared<const std::vector<unsigned char>>(MakeWav(0.55, [&](double t) {
		auto note = [&](double freq, double t0) {
			const double dt = t - t0;
			if (dt < 0.0)
				return 0.0;
			const double env = std::min(1.0, dt / 0.004) * std::exp(-dt * 8.0);
			return env * (std::sin(twoPi * freq * dt) + 0.25 * std::sin(twoPi * freq * 2.0 * dt));
		};
		return 0.24 * (note(659.25, 0.0) + note(987.77, 0.09));
	}));
#if wxUSE_SOUND && !defined(__WXMSW__)
	m_tickSound.Create(m_tickData->size(), m_tickData->data());
	m_launchSound.Create(m_launchData->size(), m_launchData->data());
#endif
}

void wxGameTileView::PlayTick()
{
	if (!m_soundEnabled)
		return;
#ifdef __WXMSW__
	if (m_tickData)
		MenuSoundWorker::Instance().Play(m_tickData);
#elif wxUSE_SOUND
	if (m_tickSound.IsOk())
		m_tickSound.Play(wxSOUND_ASYNC);
#endif
}

void wxGameTileView::PlayLaunchSound()
{
	if (!m_soundEnabled)
		return;
#ifdef __WXMSW__
	if (m_launchData)
		MenuSoundWorker::Instance().Play(m_launchData);
#elif wxUSE_SOUND
	if (m_launchSound.IsOk())
		m_launchSound.Play(wxSOUND_ASYNC);
#endif
}

void wxGameTileView::ResetLaunchState()
{
	m_launching = false;
	m_launchCalled = false;
	m_launchTitle = 0;
	m_fade = 0.0;
}

void wxGameTileView::ReloadEntries()
{
	uint64_t previousSelection = GetSelectedTitle();

	m_all = onFetchEntries ? onFetchEntries() : std::vector<wxGameTileEntry>();
	m_revision = onQueryRevision ? onQueryRevision() : 0;
	if (onLoadFavorites)
		m_favorites = onLoadFavorites();
	if (onLoadHidden)
		m_hidden = onLoadHidden();
	if (onLoadShowHidden)
		m_showHidden = onLoadShowHidden();
	m_tileSize = std::clamp(onGetTileSize ? onGetTileSize() : 1, 0, 2);
	m_accentMode = std::clamp(onGetAccent ? onGetAccent() : 0, 0, 6);

	if (m_pendingTitle != 0)
	{
		// first time the list has the remembered game: focus it
		for (const auto& e : m_all)
		{
			if (e.titleId == m_pendingTitle)
			{
				previousSelection = m_pendingTitle;
				m_pendingTitle = 0;
				break;
			}
		}
	}
	RebuildEntries(previousSelection, false);
}

// builds the list shown by the current tab out of m_all and restores the selection
void wxGameTileView::RebuildEntries(uint64_t keepTitle, bool snapScroll)
{
	InvalidateScene();
	const bool wasEmpty = m_entries.empty();
	m_entries.clear();
	m_bgBitmap = wxBitmap();
	m_bgSrcA = m_bgSrcB = nullptr;
	m_bgWeight = -1;

	// hidden games and the search filter apply to every tab
	std::vector<wxGameTileEntry> source;
	source.reserve(m_all.size());
	const wxString needle = m_searchText.Lower();
	for (const auto& e : m_all)
	{
		if (!m_showHidden && IsHidden(e.titleId))
			continue;
		if (!needle.empty() && e.name.Lower().Find(needle) == wxNOT_FOUND)
			continue;
		source.push_back(e);
	}
	if (m_tab == 0)
	{
		m_entries = source;
		SortEntries(m_entries);
	}
	else if (m_tab == 1)
	{
		for (const auto& e : source)
		{
			if (e.lastPlayedKey > 0)
				m_entries.push_back(e);
		}
		std::stable_sort(m_entries.begin(), m_entries.end(), [](const wxGameTileEntry& a, const wxGameTileEntry& b) { return a.lastPlayedKey > b.lastPlayedKey; });
		if (m_entries.size() > kMaxRecent)
			m_entries.resize(kMaxRecent);
	}
	else if (m_tab == 2)
	{
		for (const auto& e : source)
		{
			if (IsFavorite(e.titleId))
				m_entries.push_back(e);
		}
		SortEntries(m_entries);
	}
	else
	{
		// most played: only games with play time, longest first (ties by name)
		for (const auto& e : source)
		{
			if (e.minutesPlayed > 0)
				m_entries.push_back(e);
		}
		std::stable_sort(m_entries.begin(), m_entries.end(), [](const wxGameTileEntry& a, const wxGameTileEntry& b) {
			if (a.minutesPlayed != b.minutesPlayed)
				return a.minutesPlayed > b.minutesPlayed;
			return a.name.CmpNoCase(b.name) < 0;
		});
		if (m_entries.size() > kMaxMostPlayed)
			m_entries.resize(kMaxMostPlayed);
	}

	if (m_cache.size() > m_all.size() * 2 + 16)
		m_cache.clear();

	int newSelected = 0;
	if (keepTitle != 0)
	{
		for (size_t i = 0; i < m_entries.size(); ++i)
		{
			if (m_entries[i].titleId == keepTitle)
			{
				newSelected = (int)i;
				break;
			}
		}
	}
	const int maxIndex = std::max(0, (int)m_entries.size() - 1);
	m_selected = std::clamp(newSelected, 0, maxIndex);
	if (wasEmpty || snapScroll)
		m_scroll = m_selected;
	m_scroll = std::clamp(m_scroll, 0.0, (double)maxIndex);

	if (m_panelOpen && GetSelectedTitle() != m_panelTitle)
		ClosePanel();
	if (std::fabs(m_scroll - m_selected) > 0.002)
		StartAnimation();
	Refresh(false);
}

int wxGameTileView::GetSortMode() const
{
	return std::clamp(onGetSortMode ? onGetSortMode() : 0, 0, kSortModeCount - 1);
}

// order of the "Juegos" and "Favoritos" tabs: 0 name, 1 play time (most first), 2 last played (latest first); ties fall back to the name
void wxGameTileView::SortEntries(std::vector<wxGameTileEntry>& entries) const
{
	const int mode = GetSortMode();
	std::stable_sort(entries.begin(), entries.end(), [mode](const wxGameTileEntry& a, const wxGameTileEntry& b) {
		if (mode == 1 && a.minutesPlayed != b.minutesPlayed)
			return a.minutesPlayed > b.minutesPlayed;
		if (mode == 2 && a.lastPlayedKey != b.lastPlayedKey)
			return a.lastPlayedKey > b.lastPlayedKey;
		return a.name.CmpNoCase(b.name) < 0;
	});
}

bool wxGameTileView::IsHidden(uint64_t titleId) const
{
	return std::find(m_hidden.begin(), m_hidden.end(), titleId) != m_hidden.end();
}

// hides the game of the open panel (or shows it again); it leaves the lists unless "Mostrar juegos ocultos" is on
void wxGameTileView::ToggleHidden()
{
	const uint64_t titleId = m_panelTitle;
	if (titleId == 0)
		return;
	uint64_t keep = titleId;
	const auto it = std::find(m_hidden.begin(), m_hidden.end(), titleId);
	const bool hide = it == m_hidden.end();
	if (hide && !m_showHidden)
	{
		// the game disappears: move the focus to a neighbour so the list does not jump to the start
		if (m_selected + 1 < (int)m_entries.size())
			keep = m_entries[m_selected + 1].titleId;
		else if (m_selected > 0)
			keep = m_entries[m_selected - 1].titleId;
	}
	if (hide)
		m_hidden.push_back(titleId);
	else
		m_hidden.erase(it);
	if (onSaveHidden)
		onSaveHidden(m_hidden);
	ClosePanel();
	ShowToast(wxString::FromUTF8(hide ? "Juego oculto" : "Juego visible de nuevo"));
	PlayTick();
	RebuildEntries(keep, false);
}

void wxGameTileView::ToggleShowHidden()
{
	m_showHidden = !m_showHidden;
	if (onSaveShowHidden)
		onSaveShowHidden(m_showHidden);
	ShowToast(wxString::FromUTF8(m_showHidden ? "Juegos ocultos: visibles" : "Juegos ocultos: no se muestran"));
	PlayTick();
	RebuildEntries(m_panelTitle, true);
}

// quick search: "/" or Ctrl+F, the list is filtered while typing
void wxGameTileView::OpenSearch()
{
	if (m_searchActive)
		return;
	m_searchActive = true;
	InvalidateScene();
	Refresh(false);
}

void wxGameTileView::CloseSearch(bool clear)
{
	const bool hadText = !m_searchText.empty();
	m_searchActive = false;
	if (clear && hadText)
	{
		m_searchText.clear();
		ApplySearch();
		return;
	}
	InvalidateScene();
	Refresh(false);
}

void wxGameTileView::ApplySearch()
{
	RebuildEntries(GetSelectedTitle(), true);
}

void wxGameTileView::SetTab(int tab)
{
	tab = std::clamp(tab, 0, kTabCount - 1);
	if (tab == m_tab)
		return;
	const uint64_t keep = GetSelectedTitle();
	m_tab = tab;
	MarkStateDirty();
	PlayTick();
	RebuildEntries(keep, true);
}

bool wxGameTileView::IsFavorite(uint64_t titleId) const
{
	return std::find(m_favorites.begin(), m_favorites.end(), titleId) != m_favorites.end();
}

void wxGameTileView::ToggleFavorite()
{
	const uint64_t titleId = GetSelectedTitle();
	if (titleId == 0)
		return;
	InvalidateScene(); // the heart badge of the tile changes

	// in the favorites tab, move the selection to a neighbour so the list does not jump to the start
	uint64_t neighbour = 0;
	if (m_tab == 2)
	{
		if (m_selected + 1 < (int)m_entries.size())
			neighbour = m_entries[m_selected + 1].titleId;
		else if (m_selected > 0)
			neighbour = m_entries[m_selected - 1].titleId;
	}

	const auto it = std::find(m_favorites.begin(), m_favorites.end(), titleId);
	const bool added = it == m_favorites.end();
	if (added)
		m_favorites.push_back(titleId);
	else
		m_favorites.erase(it);
	if (onSaveFavorites)
		onSaveFavorites(m_favorites);

	ShowToast(wxString::FromUTF8(added ? "Agregado a favoritos" : "Quitado de favoritos"));
	PlayTick();
	if (m_tab == 2)
		RebuildEntries(added ? titleId : neighbour, false);
	else
		Refresh(false);
}

void wxGameTileView::ToggleSound()
{
	m_soundEnabled = !m_soundEnabled;
	if (onSaveSoundEnabled)
		onSaveSoundEnabled(m_soundEnabled);
	ShowToast(wxString::FromUTF8(m_soundEnabled ? "Sonido del menú: activado" : "Sonido del menú: desactivado"));
	PlayTick();
}

void wxGameTileView::ShowToast(const wxString& text)
{
	m_toast = text;
	m_toastUntilMs = std::max<long>(1, m_frameWatch.Time() + 1800);
	StartAnimation();
	Refresh(false);
}

std::vector<wxRect2DDouble> wxGameTileView::TabRects() const
{
	double tile, gap, anchorX, rowBottom;
	Metrics(tile, gap, anchorX, rowBottom);
	const double dpi = GetDPIScaleFactor();
	const wxFont font = GetFont().Scaled(1.55).Bold();
	std::vector<wxRect2DDouble> rects;
	double x = anchorX;
	for (int i = 0; i < kTabCount; ++i)
	{
		int w = 0, h = 0;
		GetTextExtent(TabName(i), &w, &h, nullptr, nullptr, &font);
		rects.emplace_back(x, 26.0 * dpi, (double)w, (double)h);
		x += w + 38.0 * dpi;
	}
	return rects;
}

int wxGameTileView::HitTestTab(const wxPoint& pos) const
{
	const double dpi = GetDPIScaleFactor();
	const auto rects = TabRects();
	for (int i = 0; i < (int)rects.size(); ++i)
	{
		const wxRect2DDouble& r = rects[i];
		const wxRect2DDouble hit(r.m_x - 8.0 * dpi, r.m_y - 8.0 * dpi, r.m_width + 16.0 * dpi, r.m_height + 22.0 * dpi);
		if (hit.Contains(wxPoint2DDouble(pos.x, pos.y)))
			return i;
	}
	return wxNOT_FOUND;
}

// quick search box of the top bar: to the right of the tabs when there is room, otherwise on its own line below them (small windows)
wxRect2DDouble wxGameTileView::SearchBoxRect() const
{
	double tile, gap, anchorX, rowBottom;
	Metrics(tile, gap, anchorX, rowBottom);
	const double dpi = GetDPIScaleFactor();
	const double W = GetClientSize().x;
	const double ty = 26.0 * dpi;
	const auto tabRects = TabRects();
	const double boxH = 36.0 * dpi;
	const double tabsEnd = tabRects.back().m_x + tabRects.back().m_width + 30.0 * dpi;
	const double clockLeft = W - anchorX * 0.7 - 110.0 * dpi;
	const double room = clockLeft - tabsEnd;
	double boxW = 300.0 * dpi;
	double boxX, boxY;
	if (room >= 220.0 * dpi)
	{
		boxW = std::min(boxW, room);
		boxX = clockLeft - boxW;
		boxY = ty - 3.0 * dpi;
	}
	else
	{
		boxW = std::min(boxW, std::max(160.0 * dpi, W - anchorX * 1.7));
		boxX = anchorX;
		boxY = ty + tabRects[0].m_height + 24.0 * dpi;
	}
	return wxRect2DDouble(boxX, boxY, boxW, boxH);
}

wxRect2DDouble wxGameTileView::HeartRect() const
{
	const wxRect2DDouble pill = PillRect();
	const double dpi = GetDPIScaleFactor();
	return wxRect2DDouble(pill.m_x + pill.m_width + 14.0 * dpi, pill.m_y, pill.m_height, pill.m_height);
}

void wxGameTileView::SelectTitle(uint64_t titleId)
{
	for (size_t i = 0; i < m_entries.size(); ++i)
	{
		if (m_entries[i].titleId == titleId)
		{
			SetSelected((int)i, false);
			return;
		}
	}
}

uint64_t wxGameTileView::GetSelectedTitle() const
{
	if (m_selected >= 0 && m_selected < (int)m_entries.size())
		return m_entries[m_selected].titleId;
	return 0;
}

void wxGameTileView::SetSelected(int index, bool playSound)
{
	if (m_entries.empty())
		return;
	index = std::clamp(index, 0, (int)m_entries.size() - 1);
	if (playSound && index != m_selected)
		PlayTick();
	if (index != m_selected)
	{
		m_pendingTitle = 0; // the user chose: no longer jump to the remembered game
		MarkStateDirty();
	}
	m_selected = index;
	StartAnimation();
	Refresh(false);
}

void wxGameTileView::MoveSelection(int delta)
{
	SetSelected(m_selected + delta);
}

void wxGameTileView::LaunchSelected()
{
	if (m_entries.empty())
	{
		if (m_all.empty() && onOpenSettings)
			onOpenSettings();
		return;
	}
	if (!onLaunch || m_launching)
		return;
	const uint64_t titleId = GetSelectedTitle();
	if (titleId == 0)
		return;

	m_lastLaunched = titleId;
	MarkStateDirty();
	SaveState(true);

	// start the fade-out; the actual launch happens once the screen is black (see OnTimer)
	m_launching = true;
	m_launchCalled = false;
	m_launchTitle = titleId;
	m_launchStartMs = m_frameWatch.Time();
	PlayLaunchSound();
	StartAnimation();
	Refresh(false);
}

void wxGameTileView::StartAnimation()
{
	if (m_animTimer.IsRunning())
		return;
	m_lastFrameMs = m_frameWatch.Time();
	m_animTimer.Start(16);
}

wxGameTileView::CacheItem& wxGameTileView::GetCache(int index)
{
	const wxGameTileEntry& entry = m_entries[index];
	auto it = m_cache.find(entry.titleId);
	if (it != m_cache.end() && it->second.iconKey == entry.iconKey)
		return it->second;

	CacheItem item;
	item.iconKey = entry.iconKey;
	wxImage img = entry.icon.IsOk() ? entry.icon.ConvertToImage() : wxImage();
	if (!img.IsOk())
	{
		img.Create(128, 128, false);
		memset(img.GetData(), 56, 128 * 128 * 3);
	}
	item.accent = DominantColour(img);
	item.tile = MakeRoundedTile(img, kTileTexSize, kTileTexSize * kCornerRadius);
	if (!entry.art || !entry.art->IsOk())
	{
		// no game art: build a colour wash from the icon (same look the view had before art support)
		wxImage amb = img.Scale(10, 6, wxIMAGE_QUALITY_HIGH).Scale(kAmbientW, kAmbientH, wxIMAGE_QUALITY_BICUBIC).Blur(2);
		wxImage wash(kAmbientW, kAmbientH, false);
		const unsigned char* src = amb.GetData();
		unsigned char* dst = wash.GetData();
		for (int y = 0; y < kAmbientH; ++y)
		{
			const double yy = y / (double)(kAmbientH - 1);
			const double base[3] = { Lerp(14, 4, yy), Lerp(19, 5, yy), Lerp(38, 11, yy) };
			for (int x = 0; x < kAmbientW; ++x)
			{
				const double xx = x / (double)(kAmbientW - 1);
				const double strength = 0.22 + 0.60 * (1.0 - yy) * (0.35 + 0.65 * xx);
				const int o = (y * kAmbientW + x) * 3;
				for (int ch = 0; ch < 3; ++ch)
					dst[o + ch] = (unsigned char)std::lround(Lerp(base[ch], src[o + ch] * 0.85, strength));
			}
		}
		item.fallbackBg = wash.Scale(kBgW, kBgH, wxIMAGE_QUALITY_BICUBIC);
	}

	CacheItem& slot = m_cache[entry.titleId];
	slot = std::move(item);
	return slot;
}

// image used as background for a game: its art if it has one, otherwise a colour wash made from its icon
const wxImage& wxGameTileView::GetBackgroundSource(int index, bool& hasArt)
{
	const wxGameTileEntry& entry = m_entries[index];
	if (entry.art && entry.art->IsOk())
	{
		hasArt = true;
		return *entry.art;
	}
	hasArt = false;
	return GetCache(index).fallbackBg;
}

// accent colour of the glow: the one taken from the game, or the fixed colour chosen in the panel
wxColour wxGameTileView::AccentFor(const wxColour& fromGame) const
{
	switch (m_accentMode)
	{
	case 1: return wxColour(70, 140, 255);
	case 2: return wxColour(150, 100, 255);
	case 3: return wxColour(255, 100, 160);
	case 4: return wxColour(70, 210, 130);
	case 5: return wxColour(255, 150, 60);
	case 6: return wxColour(235, 240, 255);
	default: return fromGame;
	}
}

void wxGameTileView::Metrics(double& tile, double& gap, double& anchorX, double& rowBottom) const
{
	const double dpi = GetDPIScaleFactor();
	const wxSize size = GetClientSize();
	const double heightDip = size.y / dpi;

	double rowBottomDip = std::min(heightDip * 0.52, heightDip - 215.0);
	rowBottomDip = std::max(rowBottomDip, 150.0);
	const double sizeFactor = m_tileSize == 0 ? 0.8 : (m_tileSize == 2 ? 1.2 : 1.0);
	double tileDip = std::min(123.0 * sizeFactor, (rowBottomDip - 96.0) / kSelectedScale);
	tileDip = std::max(tileDip, 50.0);

	tile = tileDip * dpi;
	gap = 26.0 * dpi;
	anchorX = std::max(56.0 * dpi, size.x * 0.08);
	rowBottom = rowBottomDip * dpi;
}

wxGameTileView::Geom wxGameTileView::GetGeom(int index, double scroll) const
{
	double tile, gap, anchorX, rowBottom;
	Metrics(tile, gap, anchorX, rowBottom);

	const double d = index - scroll;
	const double closeness = std::max(0.0, 1.0 - std::fabs(d));
	const double size = tile * (1.0 + (kSelectedScale - 1.0) * closeness);
	double x = anchorX + d * (tile + gap);
	if (d > 0.0)
		x += (kSelectedScale - 1.0) * tile * std::min(d, 1.0);
	return {x, rowBottom - size, size, closeness};
}

void wxGameTileView::MarkStateDirty()
{
	m_stateDirty = true;
	m_stateDirtyMs = std::max<long>(1, m_frameWatch.Time());
}

// writes the remembered state once it has been still for a moment (or right away when forced), not on every key press
void wxGameTileView::SaveState(bool force)
{
	if (!m_stateDirty || !onSaveState)
		return;
	if (!force && m_frameWatch.Time() - m_stateDirtyMs < 1500)
		return;
	m_stateDirty = false;
	wxGameTileState state;
	state.tab = m_tab;
	state.selected = m_pendingTitle != 0 ? m_pendingTitle : GetSelectedTitle();
	state.launched = m_lastLaunched;
	onSaveState(state);
}

// like a console: the mouse pointer disappears after 3 seconds without moving and comes back as soon as it moves
void wxGameTileView::UpdateCursorHide()
{
	if (m_overlayMode)
		return;
	const wxPoint mouse = wxGetMousePosition();
	const long now = m_frameWatch.Time();
	if (mouse != m_lastMousePos)
	{
		m_lastMousePos = mouse;
		m_lastMouseMs = now;
		SetInputIsPad(false);
		if (m_cursorHidden)
		{
			m_cursorHidden = false;
			SetCursor(wxNullCursor);
		}
		return;
	}
	if (!m_cursorHidden && now - m_lastMouseMs > 3000 && IsShownOnScreen() && GetScreenRect().Contains(mouse))
	{
		m_cursorHidden = true;
		SetCursor(wxCursor(wxCURSOR_BLANK));
	}
}

wxRect2DDouble wxGameTileView::PillRect() const
{
	double tile, gap, anchorX, rowBottom;
	Metrics(tile, gap, anchorX, rowBottom);
	const double dpi = GetDPIScaleFactor();
	return wxRect2DDouble(anchorX, rowBottom + 108.0 * dpi, 164.0 * dpi, 42.0 * dpi);
}

int wxGameTileView::HitTestTile(const wxPoint& pos) const
{
	for (int i = (int)m_entries.size() - 1; i >= 0; --i)
	{
		const Geom g = GetGeom(i, m_scroll);
		if (g.x > GetClientSize().x || g.x + g.size < 0)
			continue;
		if (wxRect2DDouble(g.x, g.y, g.size, g.size).Contains(wxPoint2DDouble(pos.x, pos.y)))
			return i;
	}
	return wxNOT_FOUND;
}

void wxGameTileView::OnSize(wxSizeEvent& event)
{
	InvalidateScene();
	Refresh(false);
	event.Skip();
}

void wxGameTileView::OnShow(wxShowEvent& event)
{
	if (m_overlayMode)
	{
		event.Skip();
		return; // the overlay owns the open/close state of the panel
	}
	ResetLaunchState();
	m_searchActive = false;
	m_searchText.clear();
	m_panelOpen = false;
	m_panelAnim = 0.0;
	if (event.IsShown())
	{
		if (onLoadSoundEnabled)
			m_soundEnabled = onLoadSoundEnabled();
		if (!m_stateLoaded && onLoadState)
		{
			m_stateLoaded = true;
			const wxGameTileState state = onLoadState();
			m_tab = std::clamp(state.tab, 0, kTabCount - 1);
			m_pendingTitle = state.selected;
			m_lastLaunched = state.launched;
		}
		ReloadEntries();
		m_pollTimer.Start(400);
		m_padPrimed = false;
		m_padTimer.Start(kPadPollMs);
		CallAfter([this]() { if (IsShownOnScreen()) SetFocus(); });
	}
	else
	{
		SaveState(true);
		m_pollTimer.Stop();
		m_padTimer.Stop();
		m_animTimer.Stop();
	}
	event.Skip();
}

void wxGameTileView::OnTimer(wxTimerEvent& event)
{
	if (event.GetId() == kAnimTimerId)
	{
		const long now = m_frameWatch.Time();
		const double dt = std::min(0.05, (now - m_lastFrameMs) / 1000.0);
		m_lastFrameMs = now;
		bool fullRepaint = false;

		const double target = m_selected;
		m_scroll += (target - m_scroll) * (1.0 - std::exp(-dt * 13.0));
		bool busy = true;
		if (std::fabs(target - m_scroll) < 0.002)
		{
			m_scroll = target;
			busy = false;
		}

		{
			// options panel: slide in/out + highlight and scroll smoothing
			const double panelTarget = (m_panelOpen || m_overlayMode) ? 1.0 : 0.0;
			if (std::fabs(panelTarget - m_panelAnim) > 0.003)
			{
				m_panelAnim += (panelTarget - m_panelAnim) * (1.0 - std::exp(-dt * 16.0));
				busy = true;
			}
			else
				m_panelAnim = panelTarget;
			if (m_panelOpen && m_panelSel >= 0)
			{
				double scrollTarget, hiTarget;
				PanelTargets(scrollTarget, hiTarget);
				const double k = 1.0 - std::exp(-dt * 18.0);
				m_panelScroll += (scrollTarget - m_panelScroll) * k;
				m_panelHiY += (hiTarget - m_panelHiY) * k;
				if (std::fabs(scrollTarget - m_panelScroll) > 0.5 || std::fabs(hiTarget - m_panelHiY) > 0.5)
					busy = true;
				else
				{
					m_panelScroll = scrollTarget;
					m_panelHiY = hiTarget;
				}
			}
		}

		if (m_launching)
		{
			busy = true;
			const double elapsed = (double)(now - m_launchStartMs);
			if (elapsed < kLaunchFadeMs)
				m_fade = SmoothStep(elapsed / kLaunchFadeMs);
			else
			{
				m_fade = 1.0;
				if (!m_launchCalled)
				{
					m_launchCalled = true;
					m_launchCalledMs = now;
					Refresh(false);
					Update(); // make sure the black frame is on screen before the (blocking) game start
					if (onLaunchSplash)
					{
						// cover the black gap while the game window is being set up
						for (const auto& e : m_all)
						{
							if (e.titleId == m_launchTitle)
							{
								onLaunchSplash(e.name, e.icon, e.art);
								break;
							}
						}
					}
					// the launch is posted as an event: if it succeeds this view gets destroyed, so don't touch members afterwards
					onLaunch(m_launchTitle);
					return;
				}
				if (now - m_launchCalledMs > kLaunchTimeoutMs)
				{
					// still here after the launch request -> it failed (an error dialog was shown), bring the menu back
					m_launching = false;
					m_launchCalled = false;
					if (onLaunchSplashCancel)
						onLaunchSplashCancel();
				}
			}
		}
		else if (m_fade > 0.0)
		{
			busy = true;
			m_fade = std::max(0.0, m_fade - dt * 1000.0 / kLaunchFadeInMs);
		}

		if (m_toastUntilMs > 0)
		{
			if (now < m_toastUntilMs)
				busy = true;
			else
			{
				m_toastUntilMs = 0;
				fullRepaint = true; // the toast is gone: repaint everything once so no trace of it stays
			}
		}

		if (!busy)
			m_animTimer.Stop();
		if (!m_panelOpen && m_panelAnim <= 0.0 && m_sceneValid)
		{
			// the panel is gone: free the cached picture
			m_sceneValid = false;
			m_sceneSnapshot = wxBitmap();
		}
		// with the panel fully open only its own area changes: repaint just that instead of the whole window
		if (m_sceneValid && m_panelOpen && m_panelAnim >= 1.0 && !m_overlayMode && !m_launching && m_fade <= 0.0 && m_toastUntilMs == 0 && !fullRepaint)
			RefreshRect(PanelDirtyRect(), false);
		else
			Refresh(false);
	}
	else if (event.GetId() == kPadTimerId)
	{
		UpdateCursorHide();
		PollGamepad();
	}
	else if (event.GetId() == kPollTimerId)
	{
		SaveState(false);
		if (onQueryRevision && onQueryRevision() != m_revision)
			ReloadEntries();
		else
		{
			// keep the clock in the top bar up to date
			const double dpi = GetDPIScaleFactor();
			RefreshRect(wxRect(GetClientSize().x - (int)(220 * dpi), 0, (int)(220 * dpi), (int)(90 * dpi)), false);
		}
	}
}

void wxGameTileView::OnKeyDown(wxKeyEvent& event)
{
	if (m_launching)
		return;
	if (!m_overlayMode)
		SetInputIsPad(false);
	if (m_overlayMode)
	{
		if (m_panelOpen)
			HandlePanelKey(event);
		return;
	}
	if (event.GetKeyCode() == 'M' && event.ControlDown() && !event.AltDown())
	{
		ToggleSound();
		return;
	}
	if (m_panelOpen)
	{
		HandlePanelKey(event);
		return;
	}
	if (event.GetKeyCode() == 'F' && event.ControlDown() && !event.AltDown())
	{
		OpenSearch();
		return;
	}
	if (m_searchActive)
	{
		// typing in the search box: letters arrive through OnChar, the keys below act here
		bool handled = true;
		switch (event.GetKeyCode())
		{
		case WXK_ESCAPE:
			CloseSearch(true);
			break;
		case WXK_RETURN:
		case WXK_NUMPAD_ENTER:
		case WXK_DOWN:
		case WXK_NUMPAD_DOWN:
			CloseSearch(false); // keeps the filter
			break;
		case WXK_BACK:
			if (!m_searchText.empty())
			{
				m_searchText.RemoveLast();
				ApplySearch();
			}
			break;
		case WXK_LEFT:
		case WXK_NUMPAD_LEFT:
		case WXK_RIGHT:
		case WXK_NUMPAD_RIGHT:
		case WXK_PAGEUP:
		case WXK_NUMPAD_PAGEUP:
		case WXK_PAGEDOWN:
		case WXK_NUMPAD_PAGEDOWN:
		case WXK_HOME:
		case WXK_NUMPAD_HOME:
		case WXK_END:
		case WXK_NUMPAD_END:
		case WXK_TAB:
			handled = false; // normal navigation below
			break;
		default:
			event.Skip(); // a character: OnChar adds it to the search
			return;
		}
		if (handled)
			return;
	}
	else if (event.GetKeyCode() == WXK_ESCAPE && !m_searchText.empty())
	{
		CloseSearch(true); // clears a filter that was kept
		return;
	}
	switch (event.GetKeyCode())
	{
	case WXK_LEFT:
	case WXK_NUMPAD_LEFT:
		MoveSelection(-1);
		break;
	case WXK_RIGHT:
	case WXK_NUMPAD_RIGHT:
		MoveSelection(1);
		break;
	case WXK_PAGEUP:
	case WXK_NUMPAD_PAGEUP:
		MoveSelection(-5);
		break;
	case WXK_PAGEDOWN:
	case WXK_NUMPAD_PAGEDOWN:
		MoveSelection(5);
		break;
	case WXK_HOME:
	case WXK_NUMPAD_HOME:
		SetSelected(0);
		break;
	case WXK_END:
	case WXK_NUMPAD_END:
		SetSelected((int)m_entries.size() - 1);
		break;
	case WXK_RETURN:
	case WXK_NUMPAD_ENTER:
		LaunchSelected();
		break;
	case WXK_SPACE:
		ToggleFavorite();
		break;
	case WXK_TAB:
		SetTab((m_tab + (event.ShiftDown() ? kTabCount - 1 : 1)) % kTabCount);
		break;
	case WXK_DOWN:
	case WXK_NUMPAD_DOWN:
	case WXK_MENU:
		OpenPanel();
		break;
	default:
		event.Skip();
		break;
	}
}

void wxGameTileView::OnChar(wxKeyEvent& event)
{
	if (m_launching || m_panelOpen)
		return;
	const wxUniChar c = event.GetUnicodeKey();
	if (m_searchActive)
	{
		if (!event.ControlDown() && !event.AltDown() && c.GetValue() >= 32 && c.GetValue() != 127)
		{
			m_searchText += c;
			ApplySearch();
		}
		return;
	}
	if (!event.ControlDown() && !event.AltDown() && c == '/')
	{
		OpenSearch();
		return;
	}
	// type a letter to jump to the next title starting with it
	if (event.HasModifiers() || !c.IsAscii() || !wxIsalnum(c) || m_entries.empty())
	{
		event.Skip();
		return;
	}
	const wxString letter = wxString(c).Lower();
	const int count = (int)m_entries.size();
	for (int step = 1; step <= count; ++step)
	{
		const int i = (m_selected + step) % count;
		if (m_entries[i].name.Lower().StartsWith(letter))
		{
			SetSelected(i);
			return;
		}
	}
}

void wxGameTileView::OnMouseDown(wxMouseEvent& event)
{
	SetFocus();
	if (!m_overlayMode)
		SetInputIsPad(false);
	if (m_launching || (m_overlayMode && !m_panelOpen))
		return;
	const wxPoint pos = event.GetPosition();

	if (m_panelOpen)
	{
		const int row = PanelRowAt(pos);
		if (row != wxNOT_FOUND)
		{
			m_panelSel = row;
			ActivatePanelItem(row);
		}
		else if (!PanelContains(pos))
			ClosePanel();
		return;
	}

	if (!m_all.empty() && SearchBoxRect().Contains(wxPoint2DDouble(pos.x, pos.y)))
	{
		OpenSearch(); // the box is always visible: a click starts typing
		return;
	}
	const int tabHit = HitTestTab(pos);
	if (tabHit != wxNOT_FOUND)
	{
		SetTab(tabHit);
		return;
	}
	if (m_entries.empty())
	{
		if (m_all.empty() && onOpenSettings)
			onOpenSettings();
		return;
	}
	if (HeartRect().Contains(wxPoint2DDouble(pos.x, pos.y)))
	{
		ToggleFavorite();
		return;
	}
	if (PillRect().Contains(wxPoint2DDouble(pos.x, pos.y)))
	{
		LaunchSelected();
		return;
	}
	const int hit = HitTestTile(pos);
	if (hit == wxNOT_FOUND)
		return;
	if (hit == m_selected && std::fabs(m_scroll - m_selected) < 0.05)
		LaunchSelected();
	else
		SetSelected(hit);
}

void wxGameTileView::OnMouseDClick(wxMouseEvent& event)
{
	if (m_launching || m_panelOpen || m_overlayMode)
		return;
	const int hit = HitTestTile(event.GetPosition());
	if (hit != wxNOT_FOUND)
	{
		m_selected = hit;
		LaunchSelected();
	}
}

void wxGameTileView::OnMouseRightUp(wxMouseEvent& event)
{
	if (m_launching || (m_overlayMode && !m_panelOpen))
		return;
	if (m_panelOpen)
	{
		PanelBack();
		return;
	}
	const int hit = HitTestTile(event.GetPosition());
	if (hit != wxNOT_FOUND)
	{
		SetSelected(hit);
		OpenPanel();
	}
	else if (m_entries.empty())
		OpenPanel(); // nothing to select: the panel only has the library and Cemu menu entries
	else if (onContextMenu)
		onContextMenu(0); // empty area: the classic menu (refresh list, view style)
}

void wxGameTileView::OnMouseWheel(wxMouseEvent& event)
{
	if (m_launching)
		return;
	const int wheelDelta = std::max(1, event.GetWheelDelta());
	m_wheelAccum += event.GetWheelRotation();
	const int steps = m_wheelAccum / wheelDelta;
	m_wheelAccum -= steps * wheelDelta;
	if (m_panelOpen)
	{
		for (int k = 0; k < std::abs(steps); ++k)
			MovePanelSelection(steps > 0 ? -1 : 1);
		return;
	}
	if (steps != 0)
		MoveSelection(-steps);
}

void wxGameTileView::OnMouseMove(wxMouseEvent& event)
{
	const wxPoint pos = event.GetPosition();
	if (m_panelOpen)
	{
		const int row = PanelRowAt(pos);
		if (row != wxNOT_FOUND && row != m_panelSel)
			SetPanelSelection(row);
		SetCursor(row != wxNOT_FOUND ? wxCursor(wxCURSOR_HAND) : wxNullCursor);
		event.Skip();
		return;
	}
	const bool overPill = !m_entries.empty() && PillRect().Contains(wxPoint2DDouble(pos.x, pos.y));
	const bool overHeart = !m_entries.empty() && HeartRect().Contains(wxPoint2DDouble(pos.x, pos.y));
	const bool overSearch = !m_all.empty() && SearchBoxRect().Contains(wxPoint2DDouble(pos.x, pos.y));
	const bool hand = m_all.empty() || overSearch || HitTestTab(pos) != wxNOT_FOUND || overPill || overHeart || HitTestTile(pos) != wxNOT_FOUND;
	SetCursor(hand ? wxCursor(wxCURSOR_HAND) : wxNullCursor);
	event.Skip();
}

// ================= options panel =================

double wxGameTileView::PanelRowHeight(const PanelItem& item) const
{
	if (item.id == 0 && item.label.empty())
		return 14.0 * GetDPIScaleFactor(); // separator of a menu page
	if (item.note)
		return 26.0 * GetDPIScaleFactor();
	if (item.id == kPanelDetail)
		return 38.0 * GetDPIScaleFactor();
	return (item.id == 0 ? 40.0 : 46.0) * GetDPIScaleFactor();
}

std::vector<double> wxGameTileView::PanelOffsets(double& total) const
{
	std::vector<double> offsets;
	offsets.reserve(m_panelItems.size() + 1);
	double y = 0.0;
	for (const PanelItem& item : m_panelItems)
	{
		offsets.push_back(y);
		y += PanelRowHeight(item);
	}
	offsets.push_back(y);
	total = y;
	return offsets;
}

void wxGameTileView::PanelMetrics(double& x0, double& width, double& listTop, double& listHeight) const
{
	const double dpi = GetDPIScaleFactor();
	const wxSize size = GetClientSize();
	if (m_overlayMode)
	{
		width = size.x; // the overlay window is the panel itself and slides in as a whole
		x0 = 0.0;
	}
	else
	{
		width = std::clamp(size.x * 0.30, 330.0 * dpi, 480.0 * dpi);
		x0 = size.x - width * Clamp01(m_panelAnim); // m_panelAnim already eases out, no second easing on top
	}
	listTop = 132.0 * dpi;
	listHeight = std::max(120.0 * dpi, size.y - listTop - 60.0 * dpi);
}

void wxGameTileView::PanelTargets(double& scrollTarget, double& highlightTarget) const
{
	double x0, width, listTop, listHeight, total;
	PanelMetrics(x0, width, listTop, listHeight);
	const std::vector<double> offsets = PanelOffsets(total);
	scrollTarget = 0.0;
	highlightTarget = 0.0;
	if (m_panelSel < 0 || m_panelSel >= (int)m_panelItems.size())
		return;
	highlightTarget = offsets[m_panelSel];
	const double center = (offsets[m_panelSel] + offsets[m_panelSel + 1]) / 2.0;
	scrollTarget = std::clamp(center - listHeight / 2.0, 0.0, std::max(0.0, total - listHeight));
}

bool wxGameTileView::PanelContains(const wxPoint& pos) const
{
	double x0, width, listTop, listHeight;
	PanelMetrics(x0, width, listTop, listHeight);
	return m_panelAnim > 0.003 && pos.x >= x0;
}

int wxGameTileView::PanelRowAt(const wxPoint& pos) const
{
	if (!PanelContains(pos))
		return wxNOT_FOUND;
	const double dpi = GetDPIScaleFactor();
	double x0, width, listTop, listHeight, total;
	PanelMetrics(x0, width, listTop, listHeight);
	if (pos.x < x0 + 16.0 * dpi || pos.x > x0 + width - 16.0 * dpi)
		return wxNOT_FOUND;
	if (pos.y < listTop || pos.y > listTop + listHeight)
		return wxNOT_FOUND;
	const double contentY = pos.y - listTop + m_panelScroll;
	const std::vector<double> offsets = PanelOffsets(total);
	for (size_t i = 0; i < m_panelItems.size(); ++i)
	{
		if (contentY >= offsets[i] && contentY < offsets[i + 1])
			return (m_panelItems[i].id != 0 && m_panelItems[i].enabled) ? (int)i : wxNOT_FOUND;
	}
	return wxNOT_FOUND;
}

void wxGameTileView::OpenPanel()
{
	if (m_launching || m_panelOpen)
		return;
	m_searchActive = false;
	m_scroll = m_selected; // settle the carousel: it stays still behind the panel
	InvalidateScene();
	m_panelTitle = GetSelectedTitle(); // 0 when there are no games: the panel then only has the library and Cemu menu entries
	m_panelMenuMode = false;
	m_panelConfirm = 0;
	m_panelPath.clear();
	m_panelSelStack.clear();
	BuildPanelRoot();
	m_panelOpen = true;
	FinishPanelPage(-1);
}

// overlay mode: opens the first page (called after the window is shown)
void wxGameTileView::OpenOverlayPanel()
{
	m_overlayMode = true;
	m_panelTitle = 0;
	m_panelMenuMode = false;
	m_panelConfirm = 0;
	m_panelPath.clear();
	m_panelSelStack.clear();
	BuildPanelRoot();
	m_panelOpen = true;
	m_panelAnim = 1.0;
	FinishPanelPage(-1);
}

// gamepad navigation (the overlay polls the controller and forwards the presses)
void wxGameTileView::Navigate(NavAction action)
{
	if (!m_panelOpen)
		return;
	switch (action)
	{
	case NavAction::Up:
		MovePanelSelection(-1);
		break;
	case NavAction::Down:
		MovePanelSelection(1);
		break;
	case NavAction::Left:
		if (PanelVolumeSelected())
			AdjustPanelVolume(-5);
		break;
	case NavAction::Right:
		if (PanelVolumeSelected())
			AdjustPanelVolume(5);
		else if (m_panelSel >= 0 && m_panelSel < (int)m_panelItems.size() && (m_panelItems[m_panelSel].id == kPanelSubmenu || m_panelItems[m_panelSel].id == kPanelCemuMenu || m_panelItems[m_panelSel].id == kPanelDetails))
			ActivatePanelItem(m_panelSel);
		break;
	case NavAction::Accept:
		ActivatePanelItem(m_panelSel);
		break;
	case NavAction::Back:
		PanelBack();
		break;
	}
}

// first page: options of the selected game, library entries and the Cemu menu
void wxGameTileView::BuildPanelRoot()
{
	m_panelHeadTitle.clear();
	m_panelSubtitle = wxString::FromUTF8("Opciones");
	m_panelItems.clear();
	if (m_overlayMode)
	{
		// in-game: continue, back to the library, the Cemu menu and the settings
		m_panelSubtitle = wxString::FromUTF8("Juego en pausa");
		m_panelItems.push_back({0, wxString::FromUTF8("Juego"), true});
		m_panelItems.push_back({kPanelResume, wxString::FromUTF8("Continuar"), true});
		if (onBackToLibrary)
			m_panelItems.push_back({kPanelBackToLibrary, wxString::FromUTF8("Volver a la biblioteca"), true});
		if (onQuit)
			m_panelItems.push_back({kPanelQuit, wxString::FromUTF8("Salir de Cemu"), true});
		if (onScreenshot || onSetFastForward || onSetVolume)
		{
			// quick actions
			m_panelItems.push_back({0, wxString::FromUTF8("Rápido"), true});
			if (onScreenshot)
				m_panelItems.push_back({kPanelScreenshot, wxString::FromUTF8("Captura de pantalla"), true});
			if (onSetFastForward)
			{
				PanelItem fastForward{kPanelFastForward, wxString::FromUTF8("Avance rápido"), true};
				fastForward.checkable = true;
				fastForward.checked = onGetFastForward ? onGetFastForward() : false;
				m_panelItems.push_back(fastForward);
			}
			if (onSetVolume)
				m_panelItems.push_back({kPanelVolume, wxString::FromUTF8("Volumen"), true});
		}
		if (onSetDisplayMode)
		{
			// choose how the game is shown: one of the two checks, none = normal window
			const int mode = onGetDisplayMode ? onGetDisplayMode() : 0;
			m_panelItems.push_back({0, wxString::FromUTF8("Pantalla"), true});
			PanelItem fullscreen{kPanelFullscreen, wxString::FromUTF8("Pantalla completa"), true};
			fullscreen.checkable = true;
			fullscreen.checked = mode == 1;
			m_panelItems.push_back(fullscreen);
			PanelItem borderless{kPanelBorderless, wxString::FromUTF8("Pantalla completa sin borde"), true};
			borderless.checkable = true;
			borderless.checked = mode == 2;
			m_panelItems.push_back(borderless);
		}
		m_panelItems.push_back({0, wxString::FromUTF8("Menú"), true});
		if (onFetchMenu)
			m_panelItems.push_back({kPanelCemuMenu, wxString::FromUTF8("Menú de Cemu"), true});
		if (onSetButtonStyle)
			m_panelItems.push_back({kPanelButtons, wxString(), true});
		if (onOpenSettings)
			m_panelItems.push_back({kPanelSettings, wxString::FromUTF8("Configuración de Cemu"), true});
		return;
	}
	if (m_panelTitle != 0)
	{
		m_panelItems.push_back({0, wxString::FromUTF8("Juego"), true});
		m_panelItems.push_back({kPanelStart, wxString::FromUTF8("Iniciar"), true});
		m_panelItems.push_back({kPanelFavorite, wxString(), true});
		if (onSaveHidden)
			m_panelItems.push_back({kPanelHide, wxString(), true});
		if (onFetchDetails)
			m_panelItems.push_back({kPanelDetails, wxString::FromUTF8("Detalles del juego"), true});
	}
	if (onFetchOptions)
	{
		for (const wxGameTileOption& option : onFetchOptions(m_panelTitle))
			m_panelItems.push_back({option.id, option.label, option.enabled});
	}
	m_panelItems.push_back({0, wxString::FromUTF8("Menú"), true});
	if (onFetchMenu)
		m_panelItems.push_back({kPanelCemuMenu, wxString::FromUTF8("Menú de Cemu"), true});
	if (onSetSortMode && (m_tab == 0 || m_tab == 2))
		m_panelItems.push_back({kPanelSort, wxString(), true});
	if (onSaveShowHidden)
		m_panelItems.push_back({kPanelShowHidden, wxString(), true});
	if (onSetTileSize)
		m_panelItems.push_back({kPanelTileSize, wxString(), true});
	if (onSetAccent)
		m_panelItems.push_back({kPanelAccent, wxString(), true});
	m_panelItems.push_back({kPanelSound, wxString(), true});
	if (onSetButtonStyle)
		m_panelItems.push_back({kPanelButtons, wxString(), true});
	m_panelItems.push_back({kPanelSettings, wxString::FromUTF8("Configuración de Cemu"), true});
	m_panelItems.push_back({kPanelMore, wxString::FromUTF8("Más opciones…"), true});
	if (onQuit)
		m_panelItems.push_back({kPanelQuit, wxString::FromUTF8("Salir de Cemu"), true});
}

// confirmation page: a short message and two rows, "Cancelar" (the one selected at first) and the action
void wxGameTileView::OpenConfirmPage(int what, int returnSel)
{
	m_panelConfirm = what;
	m_panelConfirmReturnSel = returnSel;
	const bool backToLibrary = what == kConfirmBackToLibrary;
	m_panelHeadTitle = wxString::FromUTF8(backToLibrary ? "¿Volver a la biblioteca?" : "¿Salir de Cemu?");
	m_panelSubtitle = wxString::FromUTF8("Confirmar");
	wxString message;
	if (backToLibrary)
		message = wxString::FromUTF8("Se cerrará el juego y Cemu volverá a abrirse en la biblioteca. Se perderá el progreso que no hayas guardado.");
	else if (m_overlayMode)
		message = wxString::FromUTF8("Se cerrará el juego y Cemu. Se perderá el progreso que no hayas guardado.");
	else
		message = wxString::FromUTF8("Se cerrará Cemu.");
	m_panelItems.clear();
	for (const wxString& line : WrapByChars(message, 30))
	{
		PanelItem note{0, line, true};
		note.note = true;
		m_panelItems.push_back(note);
	}
	m_panelItems.push_back({0, wxString(), true}); // spacer
	m_panelItems.push_back({kPanelConfirmNo, wxString::FromUTF8("Cancelar"), true});
	m_panelItems.push_back({kPanelConfirmYes, wxString::FromUTF8(backToLibrary ? "Volver a la biblioteca" : "Salir de Cemu"), true});
	FinishPanelPage(-1); // the first selectable row is "Cancelar"
}

// details page of the focused game: facts from the owner, play statistics the view already has, graphic packs and shortcuts.
// It reuses m_panelConfirm (kPageDetails) so going back returns to the first page on the row that opened it.
void wxGameTileView::OpenDetailsPage(int returnSel)
{
	if (!onFetchDetails || m_panelTitle == 0)
		return;
	const wxGameTileDetails details = onFetchDetails(m_panelTitle);
	m_panelConfirm = kPageDetails;
	m_panelConfirmReturnSel = returnSel;
	m_panelHeadTitle.clear();
	m_panelSubtitle = wxString::FromUTF8("Detalles");
	m_panelItems.clear();

	auto header = [this](const char* text) { m_panelItems.push_back({0, wxString::FromUTF8(text), true}); };
	auto row = [this](const wxString& label, const wxString& value, bool copy) {
		PanelItem item{kPanelDetail, label, true};
		item.value = value;
		item.copyOnAccept = copy;
		m_panelItems.push_back(item);
	};

	header("Información");
	for (const wxGameTileDetailRow& info : details.info)
		row(info.label, info.value, info.copy);

	wxString playtime, lastPlayed;
	for (const wxGameTileEntry& e : m_all)
	{
		if (e.titleId == m_panelTitle)
		{
			playtime = e.playtime;
			lastPlayed = e.lastPlayed;
			break;
		}
	}
	header("Partidas");
	row(wxString::FromUTF8("Tiempo jugado"), playtime.empty() ? wxString::FromUTF8("Sin jugar") : playtime, false);
	row(wxString::FromUTF8("Última partida"), lastPlayed.empty() ? wxString::FromUTF8("nunca") : lastPlayed, false);

	header("Packs gráficos activos");
	if (details.packs.empty())
		row(wxString::FromUTF8("Ninguno"), wxString(), false);
	for (const wxGameTileDetailRow& pack : details.packs)
		row(pack.label, pack.value, false);

	if (!details.actions.empty())
	{
		header("Atajos");
		for (const wxGameTileOption& action : details.actions)
			m_panelItems.push_back({action.id, action.label, action.enabled});
	}
	FinishPanelPage(-1);
}

// the list of menu entries at the level given by m_panelPath (empty path = the top level menus), null if the path is stale
const std::vector<wxGameTileMenuItem>* wxGameTileView::PanelMenuLevel() const
{
	const std::vector<wxGameTileMenuItem>* level = &m_menuTree;
	for (const int index : m_panelPath)
	{
		if (index < 0 || index >= (int)level->size())
			return nullptr;
		level = &(*level)[index].children;
	}
	return level;
}

// page of the "Menú de Cemu" at the level given by m_panelPath
void wxGameTileView::BuildMenuPage()
{
	const std::vector<wxGameTileMenuItem>* level = PanelMenuLevel();
	if (!level)
	{
		// the menu changed under us: go back to the first page
		m_panelMenuMode = false;
		m_panelPath.clear();
		m_panelSelStack.clear();
		BuildPanelRoot();
		return;
	}

	// header: name of the opened menu, and the path that leads to it
	std::vector<wxString> titles;
	titles.push_back(wxString::FromUTF8("Menú de Cemu"));
	const std::vector<wxGameTileMenuItem>* walk = &m_menuTree;
	for (const int index : m_panelPath)
	{
		titles.push_back((*walk)[index].label);
		walk = &(*walk)[index].children;
	}
	m_panelHeadTitle = titles.back();
	m_panelSubtitle.clear();
	for (size_t i = 0; i + 1 < titles.size(); ++i)
	{
		if (i > 0)
			m_panelSubtitle += wxString::FromUTF8(" \xE2\x80\xBA ");
		m_panelSubtitle += titles[i];
	}
	if (m_panelSubtitle.empty())
		m_panelSubtitle = wxString::FromUTF8("Cemu Pro");

	m_panelItems.clear();
	bool pendingSeparator = false;
	for (size_t i = 0; i < level->size(); ++i)
	{
		const wxGameTileMenuItem& entry = (*level)[i];
		if (entry.separator)
		{
			pendingSeparator = !m_panelItems.empty();
			continue;
		}
		if (pendingSeparator)
		{
			m_panelItems.push_back({0, wxString(), true}); // thin spacer between groups
			pendingSeparator = false;
		}
		PanelItem item{entry.submenu ? kPanelSubmenu : kPanelMenuCommand, entry.label, entry.enabled};
		item.sub = (int)i;
		item.cmd = entry.id;
		item.checkable = entry.checkable;
		item.radio = entry.radio;
		item.checked = entry.checked;
		m_panelItems.push_back(item);
	}
}

// selects a row (or the first selectable one) and puts scroll and highlight right on it, used after the rows were replaced
void wxGameTileView::FinishPanelPage(int preferredSel)
{
	int sel = -1;
	if (preferredSel >= 0 && preferredSel < (int)m_panelItems.size() && m_panelItems[preferredSel].id != 0 && m_panelItems[preferredSel].enabled)
		sel = preferredSel;
	else
	{
		for (int i = 0; i < (int)m_panelItems.size(); ++i)
		{
			if (m_panelItems[i].id != 0 && m_panelItems[i].enabled)
			{
				sel = i;
				break;
			}
		}
	}
	m_panelSel = sel;
	double scrollTarget, hiTarget;
	PanelTargets(scrollTarget, hiTarget);
	m_panelScroll = scrollTarget;
	m_panelHiY = hiTarget;
	PlayTick();
	StartAnimation();
	Refresh(false);
}

// rebuilds the first page keeping the selected row (a check row just changed)
void wxGameTileView::RefreshRootPage()
{
	if (m_panelMenuMode || m_panelConfirm != 0)
		return;
	const int keep = m_panelSel;
	BuildPanelRoot();
	FinishPanelPage(keep);
}

// library navigation with the controller: left / right move, accept starts the game, down or + opens the options panel,
// L / R change section, Y = favorite. While the panel is open: up / down choose, accept / back as in the in-game menu.
void wxGameTileView::PollGamepad()
{
	const wxGamepadState pad = wxReadGamepadState();
	const bool usable = !m_overlayMode && !m_launching && IsShownOnScreen() && IsEnabled() && wxTheApp && wxTheApp->IsActive();
	if (!usable || !m_padPrimed)
	{
		m_padPrev = pad; // buttons that were already held when the view became usable do nothing
		m_padPrimed = usable;
		return;
	}

	if (pad.Any() && !m_padPrev.Any())
		SetInputIsPad(true);

	const long now = std::max<long>(1, m_frameWatch.Time());
	auto repeat = [&](bool down, bool wasDown, long& next, const std::function<void()>& action) {
		if (!down)
		{
			next = 0;
			return;
		}
		if (!wasDown)
		{
			action();
			next = now + kPadRepeatDelayMs;
		}
		else if (now >= next)
		{
			action();
			next = now + kPadRepeatIntervalMs;
		}
	};

	if (m_panelOpen)
	{
		if (pad.accept && !m_padPrev.accept)
			Navigate(NavAction::Accept);
		else if (pad.back && !m_padPrev.back)
			Navigate(NavAction::Back);
		repeat(pad.up, m_padPrev.up, m_padNextUp, [this]() { Navigate(NavAction::Up); });
		repeat(pad.down, m_padPrev.down, m_padNextDown, [this]() { Navigate(NavAction::Down); });
	}
	else
	{
		if (pad.accept && !m_padPrev.accept)
			LaunchSelected();
		else if ((pad.down && !m_padPrev.down) || (pad.start && !m_padPrev.start))
		{
			OpenPanel();
			m_padNextUp = m_padNextDown = now + kPadRepeatDelayMs; // the press that opened the panel must not also move inside it
		}
		else
		{
			if (pad.favorite && !m_padPrev.favorite)
				ToggleFavorite();
			if (pad.tabNext && !m_padPrev.tabNext)
				SetTab((m_tab + 1) % kTabCount);
			if (pad.tabPrev && !m_padPrev.tabPrev)
				SetTab((m_tab + kTabCount - 1) % kTabCount);
			repeat(pad.left, m_padPrev.left, m_padNextLeft, [this]() { MoveSelection(-1); });
			repeat(pad.right, m_padPrev.right, m_padNextRight, [this]() { MoveSelection(1); });
		}
	}
	m_padPrev = pad;
}

// reloads the current menu page (a check or radio entry just changed) keeping the selected row
void wxGameTileView::RefreshMenuPage()
{
	const int keep = m_panelSel;
	m_menuTree = onFetchMenu ? onFetchMenu() : std::vector<wxGameTileMenuItem>();
	BuildMenuPage();
	FinishPanelPage(keep);
}

// one page back; closes the panel when already on the first page
void wxGameTileView::PanelBack()
{
	if (m_panelConfirm != 0)
	{
		// cancel the confirmation: back to the first page, on the row that opened it
		m_panelConfirm = 0;
		BuildPanelRoot();
		FinishPanelPage(m_panelConfirmReturnSel);
		return;
	}
	if (!m_panelMenuMode)
	{
		ClosePanel();
		return;
	}
	int restore = -1;
	if (!m_panelSelStack.empty())
	{
		restore = m_panelSelStack.back();
		m_panelSelStack.pop_back();
	}
	if (m_panelPath.empty())
	{
		m_panelMenuMode = false;
		BuildPanelRoot();
	}
	else
	{
		m_panelPath.pop_back();
		BuildMenuPage();
	}
	FinishPanelPage(restore);
}

void wxGameTileView::ClosePanel()
{
	if (!m_panelOpen)
		return;
	m_panelOpen = false;
	m_panelConfirm = 0;
	if (m_overlayMode)
	{
		if (onPanelClosed)
			onPanelClosed(); // the overlay window animates itself out
		return;
	}
	StartAnimation();
	Refresh(false);
}

void wxGameTileView::SetPanelSelection(int index)
{
	if (index < 0 || index >= (int)m_panelItems.size() || index == m_panelSel)
		return;
	m_panelSel = index;
	PlayTick();
	StartAnimation();
	Refresh(false);
}

// moves to the next selectable row (headers and disabled rows are skipped)
void wxGameTileView::MovePanelSelection(int dir)
{
	for (int i = m_panelSel + dir; i >= 0 && i < (int)m_panelItems.size(); i += dir)
	{
		if (m_panelItems[i].id != 0 && m_panelItems[i].enabled)
		{
			SetPanelSelection(i);
			return;
		}
	}
}

void wxGameTileView::ActivatePanelItem(int index)
{
	if (index < 0 || index >= (int)m_panelItems.size())
		return;
	const PanelItem item = m_panelItems[index]; // copy, the list may change while the action runs
	if (item.id == 0 || !item.enabled)
		return;
	const uint64_t titleId = m_panelTitle;
	switch (item.id)
	{
	case kPanelStart:
		ClosePanel();
		LaunchSelected();
		break;
	case kPanelFavorite:
		ToggleFavorite();
		if (m_tab == 2)
			ClosePanel(); // the game leaves this list when it is removed
		break;
	case kPanelSound:
		ToggleSound();
		break;
	case kPanelSettings:
		if (!m_overlayMode)
			ClosePanel(); // the overlay hides itself around the action
		if (onOpenSettings)
			onOpenSettings();
		break;
	case kPanelResume:
		ClosePanel();
		break;
	case kPanelBackToLibrary:
		OpenConfirmPage(kConfirmBackToLibrary, index);
		break;
	case kPanelQuit:
		OpenConfirmPage(kConfirmQuit, index);
		break;
	case kPanelConfirmNo:
		PanelBack();
		break;
	case kPanelConfirmYes:
	{
		const int what = m_panelConfirm;
		if (!m_overlayMode)
			ClosePanel(); // the overlay hides itself around the action
		if (what == kConfirmBackToLibrary && onBackToLibrary)
			onBackToLibrary();
		else if (what == kConfirmQuit && onQuit)
			onQuit();
		break;
	}
	case kPanelDetails:
		OpenDetailsPage(index);
		break;
	case kPanelDetail:
		if (item.copyOnAccept && wxTheClipboard->Open())
		{
			wxTheClipboard->SetData(new wxTextDataObject(item.value));
			wxTheClipboard->Close();
			ShowToast(wxString::FromUTF8("Copiado: ") + item.value);
			PlayTick();
		}
		break;
	case kPanelFullscreen:
	case kPanelBorderless:
	{
		// check rows: choosing the one that is already on goes back to the normal window
		const int wanted = item.id == kPanelFullscreen ? 1 : 2;
		const int current = onGetDisplayMode ? onGetDisplayMode() : 0;
		if (onSetDisplayMode)
			onSetDisplayMode(current == wanted ? 0 : wanted);
		RefreshRootPage();
		break;
	}
	case kPanelHide:
		ToggleHidden();
		break;
	case kPanelShowHidden:
		ToggleShowHidden();
		break;
	case kPanelSort:
	{
		// name -> play time -> last played -> name; the focused game stays focused
		const int next = (GetSortMode() + 1) % kSortModeCount;
		if (onSetSortMode)
			onSetSortMode(next);
		ShowToast(wxString::FromUTF8("Orden: ") + wxString::FromUTF8(SortModeName(next)));
		PlayTick();
		RebuildEntries(m_panelTitle, true);
		break;
	}
	case kPanelTileSize:
	{
		// normal -> large -> small -> normal
		static const int order[3] = {1, 2, 0};
		const int next = order[((m_tileSize == 1 ? 0 : (m_tileSize == 2 ? 1 : 2)) + 1) % 3];
		m_tileSize = next;
		if (onSetTileSize)
			onSetTileSize(next);
		ShowToast(wxString::FromUTF8("Tamaño de tarjetas: ") + wxString::FromUTF8(TileSizeName(next)));
		PlayTick();
		InvalidateScene();
		Refresh(false);
		break;
	}
	case kPanelAccent:
	{
		m_accentMode = (m_accentMode + 1) % 7;
		if (onSetAccent)
			onSetAccent(m_accentMode);
		ShowToast(wxString::FromUTF8("Color de acento: ") + wxString::FromUTF8(AccentName(m_accentMode)));
		PlayTick();
		InvalidateScene();
		Refresh(false);
		break;
	}
	case kPanelButtons:
		// automatic -> PlayStation -> Xbox -> Nintendo -> automatic
		if (onSetButtonStyle)
			onSetButtonStyle(((onGetButtonStyle ? onGetButtonStyle() : 0) + 1) % 4);
		PlayTick();
		InvalidateScene();
		Refresh(false);
		break;
	case kPanelScreenshot:
		if (onScreenshot)
			onScreenshot();
		break;
	case kPanelFastForward:
		if (onSetFastForward)
			onSetFastForward(!(onGetFastForward && onGetFastForward()));
		RefreshRootPage();
		break;
	case kPanelVolume:
		// accept steps the volume up by 10 and wraps around to 0 (left / right fine tune it)
		AdjustPanelVolume((onGetVolume && onGetVolume() >= 100) ? -100 : 10);
		break;
	case kPanelMore:
		ClosePanel();
		if (onContextMenu)
			onContextMenu(titleId);
		break;
	case kPanelCemuMenu:
		m_menuTree = onFetchMenu ? onFetchMenu() : std::vector<wxGameTileMenuItem>();
		m_panelMenuMode = true;
		m_panelPath.clear();
		m_panelSelStack.assign(1, index); // row to come back to
		BuildMenuPage();
		FinishPanelPage(-1);
		break;
	case kPanelSubmenu:
		m_panelSelStack.push_back(index);
		m_panelPath.push_back(item.sub);
		BuildMenuPage();
		FinishPanelPage(-1);
		break;
	case kPanelMenuCommand:
		if (item.checkable)
		{
			// check and radio entries keep the panel open so the new state is visible
			if (onMenuCommand)
				onMenuCommand(item.cmd);
			RefreshMenuPage();
		}
		else
		{
			if (!m_overlayMode)
				ClosePanel(); // the overlay hides itself around the command
			if (onMenuCommand)
				onMenuCommand(item.cmd);
		}
		break;
	default:
		ClosePanel();
		if (onOption)
			onOption(titleId, item.id);
		break;
	}
}

int wxGameTileView::ResolveButtonStyle()
{
	const int setting = onGetButtonStyle ? onGetButtonStyle() : 0;
	if (setting >= 1 && setting <= 3)
		return setting - 1;
	const long now = m_frameWatch.Time();
	if (now - m_autoStyleMs > 2000)
	{
		m_autoStyle = wxDetectGamepadStyle();
		m_autoStyleMs = now;
	}
	return m_autoStyle;
}

void wxGameTileView::SetInputIsPad(bool pad)
{
	if (m_inputIsPad == pad)
		return;
	m_inputIsPad = pad;
	InvalidateScene(); // the footer of the library changes
	Refresh(false);
}

// hint line of the options panel
wxString wxGameTileView::PanelHintText()
{
	const PadLabels labels = GetPadLabels(ResolveButtonStyle());
	if (m_overlayMode)
		return wxString::FromUTF8("Cruceta  Elegir      ") + wxString::FromUTF8(labels.accept) + wxString::FromUTF8("  Aceptar      ") + wxString::FromUTF8(labels.back) + wxString::FromUTF8("  Volver");
	if (m_inputIsPad)
		return wxString::FromUTF8("\xE2\x86\x91 \xE2\x86\x93  Elegir      ") + wxString::FromUTF8(labels.accept) + wxString::FromUTF8("  Aceptar      ") + wxString::FromUTF8(labels.back) + wxString::FromUTF8("  Volver");
	return wxString::FromUTF8("\xE2\x86\x91 \xE2\x86\x93  Elegir      Enter  Aceptar      Esc  Volver");
}

// footer of the library while the controller is the active input
wxString wxGameTileView::LibraryPadHintText()
{
	const PadLabels labels = GetPadLabels(ResolveButtonStyle());
	return wxString::FromUTF8("\xE2\x86\x90  \xE2\x86\x92  Explorar      ") + wxString::FromUTF8(labels.accept) + wxString::FromUTF8("  Iniciar      ")
		+ wxString::FromUTF8(labels.tabs) + wxString::FromUTF8("  Secciones      ") + wxString::FromUTF8(labels.favorite) + wxString::FromUTF8("  Favorito      \xE2\x86\x93  Opciones");
}

bool wxGameTileView::PanelVolumeSelected() const
{
	return m_panelSel >= 0 && m_panelSel < (int)m_panelItems.size() && m_panelItems[m_panelSel].id == kPanelVolume;
}

void wxGameTileView::AdjustPanelVolume(int delta)
{
	if (!onGetVolume || !onSetVolume)
		return;
	const int volume = std::clamp(onGetVolume() + delta, 0, 100);
	onSetVolume(volume);
	PlayTick();
	Refresh(false);
}

void wxGameTileView::HandlePanelKey(wxKeyEvent& event)
{
	switch (event.GetKeyCode())
	{
	case WXK_UP:
	case WXK_NUMPAD_UP:
		MovePanelSelection(-1);
		break;
	case WXK_DOWN:
	case WXK_NUMPAD_DOWN:
		MovePanelSelection(1);
		break;
	case WXK_PAGEUP:
	case WXK_NUMPAD_PAGEUP:
		for (int k = 0; k < 5; ++k)
			MovePanelSelection(-1);
		break;
	case WXK_PAGEDOWN:
	case WXK_NUMPAD_PAGEDOWN:
		for (int k = 0; k < 5; ++k)
			MovePanelSelection(1);
		break;
	case WXK_HOME:
	case WXK_NUMPAD_HOME:
		for (int k = 0; k < (int)m_panelItems.size(); ++k)
			MovePanelSelection(-1);
		break;
	case WXK_END:
	case WXK_NUMPAD_END:
		for (int k = 0; k < (int)m_panelItems.size(); ++k)
			MovePanelSelection(1);
		break;
	case WXK_RETURN:
	case WXK_NUMPAD_ENTER:
	case WXK_SPACE:
		ActivatePanelItem(m_panelSel);
		break;
	case WXK_RIGHT:
	case WXK_NUMPAD_RIGHT:
		if (PanelVolumeSelected())
		{
			AdjustPanelVolume(5);
			break;
		}
		// entries that open a page can also be entered with the right arrow
		if (m_panelSel >= 0 && m_panelSel < (int)m_panelItems.size() && (m_panelItems[m_panelSel].id == kPanelSubmenu || m_panelItems[m_panelSel].id == kPanelCemuMenu || m_panelItems[m_panelSel].id == kPanelDetails))
			ActivatePanelItem(m_panelSel);
		break;
	case WXK_LEFT:
	case WXK_NUMPAD_LEFT:
		if (PanelVolumeSelected())
		{
			AdjustPanelVolume(-5);
			break;
		}
		PanelBack();
		break;
	case WXK_ESCAPE:
	case WXK_BACK:
		PanelBack(); // one page back, closes the panel from the first page
		break;
	case WXK_MENU:
		ClosePanel();
		break;
	default:
		break; // swallow everything else while the panel is open
	}
}

void wxGameTileView::RenderPanel(wxGraphicsContext& gc, wxDC& dc)
{
	if (m_panelAnim <= 0.003 || m_panelItems.empty())
		return;

	const double dpi = GetDPIScaleFactor();
	const wxSize size = GetClientSize();
	const double W = size.x;
	const double H = size.y;
	const double ease = Clamp01(m_panelAnim);
	double x0, width, listTop, listHeight, total;
	PanelMetrics(x0, width, listTop, listHeight);
	const std::vector<double> offsets = PanelOffsets(total);

	const wxColour white(255, 255, 255);
	const wxColour textDim(170, 178, 200);
	const wxColour dark(14, 18, 32);
	const double pad = 28.0 * dpi;

	// fonts and measured text heights are cached: measuring text is the slowest part of drawing the panel
	if (m_panelFontDpi != dpi)
	{
		m_panelFontDpi = dpi;
		m_panelTitleFont = GetFont().Scaled(1.3).Bold();
		m_panelSubFont = GetFont().Scaled(1.0);
		m_panelRowFont = GetFont().Scaled(1.25);
		m_panelHeaderFont = GetFont().Scaled(0.95).Bold();
		m_panelHeadW = -1;
		wxDouble mw, mh;
		gc.SetFont(m_panelTitleFont, white);
		gc.GetTextExtent(wxString::FromUTF8("Ag"), &mw, &mh);
		m_panelHeadLineH = mh;
		gc.SetFont(m_panelSubFont, textDim);
		gc.GetTextExtent(wxString::FromUTF8("Ag"), &mw, &mh);
		m_panelHeadSubH = mh;
		gc.SetFont(m_panelRowFont, white);
		gc.GetTextExtent(wxString::FromUTF8("Ag"), &mw, &mh);
		m_panelRowTextH = mh;
		gc.SetFont(m_panelHeaderFont, textDim);
		gc.GetTextExtent(wxString::FromUTF8("Ag"), &mw, &mh);
		m_panelHeaderTextH = mh;
	}

	// dim the carousel, soft shadow, panel background
	gc.SetPen(*wxTRANSPARENT_PEN);
	gc.SetBrush(wxBrush(wxColour(0, 0, 0, (unsigned char)std::lround(130.0 * ease))));
	gc.DrawRectangle(0, 0, std::max(0.0, x0), H);
	const double shadowW = 56.0 * dpi;
	gc.SetBrush(gc.CreateLinearGradientBrush(x0 - shadowW, 0, x0, 0, wxColour(0, 0, 0, 0), wxColour(0, 0, 0, (unsigned char)std::lround(150.0 * ease))));
	gc.DrawRectangle(x0 - shadowW, 0, shadowW, H);
	gc.SetBrush(wxBrush(wxColour(10, 13, 24, 218)));
	gc.DrawRectangle(x0, 0, width + 2.0, H);

	// header: cover + game name on the first page, name of the opened menu on the menu pages
	const bool hasGame = !m_panelMenuMode && (m_panelConfirm == 0 || m_panelConfirm == kPageDetails) && m_panelTitle != 0 && m_selected >= 0 && m_selected < (int)m_entries.size();
	const double icon = 64.0 * dpi;
	const double headY = 30.0 * dpi;
	wxString headTitle = (m_panelMenuMode || m_panelConfirm != 0) ? m_panelHeadTitle : wxString::FromUTF8("Cemu Pro");
	double textX = x0 + pad;
	if (hasGame)
	{
		const wxGameTileEntry& entry = m_entries[m_selected];
		const CacheItem& cache = GetCache(m_selected);
		if (cache.tile.IsOk())
			gc.DrawBitmap(cache.tile, x0 + pad, headY, icon, icon);
		headTitle = entry.name;
		textX = x0 + pad + icon + 16.0 * dpi;
	}
	{
		const int maxW = (int)(width - (textX - x0) - pad);
		// long names use up to two lines instead of being cut off (wrapped only when the name or the width changes)
		if (m_panelHeadW != maxW || m_panelHeadKey != headTitle)
		{
			dc.SetFont(m_panelTitleFont);
			m_panelHeadLines = WrapText(headTitle, dc, maxW, 2);
			m_panelHeadW = maxW;
			m_panelHeadKey = headTitle;
		}
		const double th = m_panelHeadLineH;
		const double lineGap = 2.0 * dpi;
		const double blockH = th * (double)m_panelHeadLines.size() + lineGap + m_panelHeadSubH;
		double textY = headY + (icon - blockH) / 2.0;
		gc.SetFont(m_panelTitleFont, white);
		for (const wxString& line : m_panelHeadLines)
		{
			gc.DrawText(line, textX, textY);
			textY += th;
		}
		gc.SetFont(m_panelSubFont, textDim);
		gc.DrawText(m_panelSubtitle, textX, textY + lineGap);
	}
	gc.SetPen(wxPen(WithAlpha(white, 0.14), 1));
	gc.StrokeLine(x0 + pad, listTop - 16.0 * dpi, x0 + width - pad, listTop - 16.0 * dpi);

	// list
	const wxFont& rowFont = m_panelRowFont;
	const wxFont& headerFont = m_panelHeaderFont;
	gc.PushState(); // keeps the clipping the paint handler set, ResetClip would remove it
	gc.Clip(x0, listTop, width, listHeight);

	if (m_panelSel >= 0 && m_panelSel < (int)m_panelItems.size())
	{
		const double hiY = listTop + m_panelHiY - m_panelScroll;
		const double hiH = PanelRowHeight(m_panelItems[m_panelSel]);
		gc.SetPen(*wxTRANSPARENT_PEN);
		gc.SetBrush(wxBrush(wxColour(255, 255, 255, 245)));
		gc.DrawRoundedRectangle(x0 + 16.0 * dpi, hiY + 2.0 * dpi, width - 32.0 * dpi, hiH - 4.0 * dpi, 10.0 * dpi);
	}

	for (size_t i = 0; i < m_panelItems.size(); ++i)
	{
		PanelItem& item = m_panelItems[i];
		const double top = listTop + offsets[i] - m_panelScroll;
		const double rowH = offsets[i + 1] - offsets[i];
		if (top + rowH < listTop || top > listTop + listHeight)
			continue;

		if (item.id == 0)
		{
			if (item.label.empty())
				continue; // spacer between groups of a menu page
			if (item.note)
			{
				gc.SetFont(m_panelSubFont, textDim);
				gc.DrawText(item.label, x0 + pad, top + (rowH - m_panelHeadSubH) / 2.0);
				continue;
			}
			gc.SetFont(headerFont, WithAlpha(textDim, 0.85));
			const wxString text = item.label.Upper();
			gc.DrawText(text, x0 + pad, top + rowH - m_panelHeaderTextH - 6.0 * dpi);
			continue;
		}

		wxString label = item.label;
		wxString value;
		double valueReserve = 0.0; // width taken by the value on the right, the label gets what is left
		if (item.id == kPanelDetail)
		{
			const int valueMax = (int)((width - pad * 2.0) * 0.55);
			if (item.shownValueW != valueMax || item.shownValueKey != item.value)
			{
				dc.SetFont(rowFont);
				item.shownValue = wxControl::Ellipsize(item.value, dc, wxELLIPSIZE_END, valueMax);
				item.shownValueKey = item.value;
				item.shownValueW = valueMax;
				item.valueTextW = item.shownValue.empty() ? 0 : dc.GetTextExtent(item.shownValue).x;
			}
			value = item.shownValue;
			valueReserve = item.valueTextW > 0 ? item.valueTextW + 16.0 * dpi : 0.0;
		}
		else if (item.id == kPanelFavorite)
			label = wxString::FromUTF8(IsFavorite(m_panelTitle) ? "Quitar de favoritos" : "Agregar a favoritos");
		else if (item.id == kPanelHide)
			label = wxString::FromUTF8(IsHidden(m_panelTitle) ? "Dejar de ocultar juego" : "Ocultar juego");
		else if (item.id == kPanelShowHidden)
		{
			label = wxString::FromUTF8("Mostrar juegos ocultos");
			value = wxString::FromUTF8(m_showHidden ? "Activado" : "Desactivado");
		}
		else if (item.id == kPanelSort)
		{
			label = wxString::FromUTF8("Ordenar juegos");
			value = wxString::FromUTF8(SortModeName(GetSortMode()));
		}
		else if (item.id == kPanelTileSize)
		{
			label = wxString::FromUTF8("Tamaño de tarjetas");
			value = wxString::FromUTF8(TileSizeName(m_tileSize));
		}
		else if (item.id == kPanelAccent)
		{
			label = wxString::FromUTF8("Color de acento");
			value = wxString::FromUTF8(AccentName(m_accentMode));
		}
		else if (item.id == kPanelButtons)
		{
			label = wxString::FromUTF8("Botones del mando");
			value = wxString::FromUTF8(ButtonStyleName(onGetButtonStyle ? onGetButtonStyle() : 0));
		}
		else if (item.id == kPanelVolume)
			value = wxString::FromUTF8("\xE2\x80\xB9  ") + wxString::Format("%d%%", onGetVolume ? onGetVolume() : 0) + wxString::FromUTF8("  \xE2\x80\xBA");
		else if (item.id == kPanelSound)
		{
			label = wxString::FromUTF8("Sonido del menú");
			value = wxString::FromUTF8(m_soundEnabled ? "Activado" : "Desactivado");
		}

		const bool selected = (int)i == m_panelSel;
		const wxColour textColour = selected ? dark : (item.enabled ? white : WithAlpha(white, 0.35));

		// long entries (menu items, recent file paths) are cut with an ellipsis, leaving room for the indicator on the right
		const bool opensPage = item.id == kPanelSubmenu || item.id == kPanelCemuMenu || item.id == kPanelDetails;
		const bool hasIndicator = opensPage || item.checkable || item.id == kPanelFavorite;
		const double maxLabelW = width - pad * 2.0 - (hasIndicator ? 34.0 * dpi : 0.0) - valueReserve;
		const int labelMode = item.id == kPanelMenuCommand ? 1 : 0;
		if (item.shownW != (int)maxLabelW || item.shownMode != labelMode || item.shownKey != label)
		{
			dc.SetFont(rowFont);
			item.shown = wxControl::Ellipsize(label, dc, labelMode ? wxELLIPSIZE_MIDDLE : wxELLIPSIZE_END, (int)maxLabelW);
			item.shownKey = label;
			item.shownW = (int)maxLabelW;
			item.shownMode = labelMode;
		}

		gc.SetFont(rowFont, textColour);
		gc.DrawText(item.shown, x0 + pad, top + (rowH - m_panelRowTextH) / 2.0);

		const double rightX = x0 + width - pad;
		const double midY = top + rowH / 2.0;
		const int lineW = std::max(1, (int)std::lround(2.0 * dpi));
		if (opensPage)
		{
			// chevron
			const double s = 5.0 * dpi;
			wxGraphicsPath chevron = gc.CreatePath();
			chevron.MoveToPoint(rightX - s * 1.6, midY - s);
			chevron.AddLineToPoint(rightX - s * 0.6, midY);
			chevron.AddLineToPoint(rightX - s * 1.6, midY + s);
			gc.SetBrush(*wxTRANSPARENT_BRUSH);
			gc.SetPen(wxPen(textColour, lineW));
			gc.StrokePath(chevron);
		}
		else if (item.checkable && item.radio)
		{
			const double s = 6.0 * dpi;
			gc.SetBrush(*wxTRANSPARENT_BRUSH);
			gc.SetPen(wxPen(textColour, std::max(1, (int)std::lround(1.6 * dpi))));
			gc.DrawEllipse(rightX - s * 2.0, midY - s, s * 2.0, s * 2.0);
			if (item.checked)
			{
				gc.SetPen(*wxTRANSPARENT_PEN);
				gc.SetBrush(wxBrush(textColour));
				gc.DrawEllipse(rightX - s * 1.5, midY - s * 0.5, s, s);
			}
		}
		else if (item.checkable && item.checked)
		{
			// check mark
			const double s = 6.0 * dpi;
			wxGraphicsPath tick = gc.CreatePath();
			tick.MoveToPoint(rightX - s * 2.0, midY);
			tick.AddLineToPoint(rightX - s * 1.1, midY + s * 0.8);
			tick.AddLineToPoint(rightX - s * 0.1, midY - s * 0.8);
			gc.SetBrush(*wxTRANSPARENT_BRUSH);
			gc.SetPen(wxPen(textColour, lineW));
			gc.StrokePath(tick);
		}
		if (!value.empty())
		{
			wxDouble vw, vh;
			gc.GetTextExtent(value, &vw, &vh);
			gc.SetFont(rowFont, selected ? WithAlpha(dark, 0.7) : textDim);
			gc.DrawText(value, rightX - vw, top + (rowH - vh) / 2.0);
		}
		if (item.id == kPanelFavorite)
		{
			const wxGraphicsPath heart = MakeHeartPath(gc, rightX - 8.0 * dpi, top + rowH / 2.0, 7.0 * dpi);
			if (IsFavorite(m_panelTitle))
			{
				gc.SetPen(*wxTRANSPARENT_PEN);
				gc.SetBrush(wxBrush(wxColour(255, 92, 128)));
				gc.FillPath(heart);
			}
			else
			{
				gc.SetBrush(*wxTRANSPARENT_BRUSH);
				gc.SetPen(wxPen(textColour, std::max(1, (int)std::lround(1.6 * dpi))));
				gc.StrokePath(heart);
			}
		}
	}
	gc.PopState();

	// thin scrollbar when the list does not fit
	if (total > listHeight)
	{
		const double thumbH = std::max(24.0 * dpi, listHeight * listHeight / total);
		const double maxScroll = total - listHeight;
		const double thumbY = listTop + (maxScroll > 0 ? (m_panelScroll / maxScroll) : 0.0) * (listHeight - thumbH);
		gc.SetPen(*wxTRANSPARENT_PEN);
		gc.SetBrush(wxBrush(wxColour(255, 255, 255, 70)));
		gc.DrawRoundedRectangle(x0 + width - 9.0 * dpi, thumbY, 3.0 * dpi, thumbH, 1.5 * dpi);
	}

	// hint line
	gc.SetFont(m_panelSubFont, WithAlpha(textDim, 0.8));
	gc.DrawText(PanelHintText(), x0 + pad, H - 38.0 * dpi);
}

void wxGameTileView::InvalidateScene()
{
	m_sceneValid = false;
}

// area the options panel needs while it is fully open: the panel itself and the shadow it casts on the carousel
wxRect wxGameTileView::PanelDirtyRect() const
{
	double x0, width, listTop, listHeight;
	PanelMetrics(x0, width, listTop, listHeight);
	const double shadowW = 56.0 * GetDPIScaleFactor();
	const int left = std::max(0, (int)std::floor(x0 - shadowW) - 2);
	const wxSize size = GetClientSize();
	return wxRect(left, 0, std::max(1, size.x - left), size.y);
}

// copies what is currently drawn (the scene, without the panel) so the carousel does not have to be drawn again while the panel animates
void wxGameTileView::CaptureSceneSnapshot(wxDC& source, const wxSize& size)
{
	wxBitmap bitmap(size.x, size.y, 24);
	if (!bitmap.IsOk())
		return;
	{
		wxMemoryDC memory(bitmap);
		memory.Blit(0, 0, size.x, size.y, &source, 0, 0);
	}
	m_sceneSnapshot = bitmap;
	m_sceneValid = true;
}

void wxGameTileView::OnPaint(wxPaintEvent&)
{
	wxAutoBufferedPaintDC dc(this);
	const wxSize size = GetClientSize();
	if (size.x <= 0 || size.y <= 0)
		return;

	// only the invalidated area is drawn (the clock refresh and the panel highlight no longer repaint the whole window)
	const wxRect client(0, 0, size.x, size.y);
	wxRect box = GetUpdateClientRect().Intersect(client);
	if (box.IsEmpty())
		box = client;
	const bool fullPaint = box == client;

	auto makeContext = [&]() {
		std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
		if (gc)
		{
			gc->SetInterpolationQuality(wxINTERPOLATION_BEST);
			gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
			if (!fullPaint)
				gc->Clip(box.x, box.y, box.width, box.height);
		}
		return gc;
	};

	const bool settled = !m_entries.empty() && m_fade <= 0.0 && std::fabs(m_scroll - m_selected) < 0.002;
	const bool panelShown = m_panelOpen || m_panelAnim > 0.003;
	const bool useSnapshot = !m_overlayMode && settled && panelShown && m_sceneValid && m_sceneSnapshot.IsOk() &&
							 m_sceneSnapshot.GetWidth() == size.x && m_sceneSnapshot.GetHeight() == size.y;
	const bool captureSnapshot = !m_overlayMode && settled && m_panelOpen && !m_sceneValid && fullPaint;

	if (useSnapshot)
	{
		// the carousel behind the panel is a still picture: one cheap copy instead of drawing every tile again
		std::unique_ptr<wxGraphicsContext> gc = makeContext();
		if (!gc)
			return;
		gc->SetInterpolationQuality(wxINTERPOLATION_NONE);
		gc->DrawBitmap(m_sceneSnapshot, 0, 0, size.x, size.y);
		gc->SetInterpolationQuality(wxINTERPOLATION_BEST);
		RenderTop(*gc, dc);
	}
	else if (captureSnapshot)
	{
		{
			std::unique_ptr<wxGraphicsContext> gc = makeContext();
			if (!gc)
				return;
			RenderScene(*gc, dc);
		} // the context is released here, so everything is in the DC before it is copied
		CaptureSceneSnapshot(dc, size);
		std::unique_ptr<wxGraphicsContext> gc = makeContext();
		if (!gc)
			return;
		RenderTop(*gc, dc);
	}
	else
	{
		std::unique_ptr<wxGraphicsContext> gc = makeContext();
		if (!gc)
			return;
		Render(*gc, dc);
	}
}

// the whole picture in one go (in-game overlay, or when there is no cached scene)
void wxGameTileView::Render(wxGraphicsContext& gc, wxDC& dc)
{
	if (m_overlayMode)
	{
		const wxSize size = GetClientSize();
		if (size.x <= 0 || size.y <= 0)
			return;
		gc.SetInterpolationQuality(wxINTERPOLATION_BEST);
		gc.SetAntialiasMode(wxANTIALIAS_DEFAULT);
		gc.SetPen(*wxTRANSPARENT_PEN);
		gc.SetBrush(wxBrush(wxColour(8, 10, 18)));
		gc.DrawRectangle(0, 0, size.x, size.y);
		RenderPanel(gc, dc);
		return;
	}
	RenderScene(gc, dc);
	RenderTop(gc, dc);
}

// background, tabs, tiles, title and footer: everything that stays the same while the options panel is open
void wxGameTileView::RenderScene(wxGraphicsContext& gc, wxDC& dc)
{
	gc.SetInterpolationQuality(wxINTERPOLATION_BEST);
	gc.SetAntialiasMode(wxANTIALIAS_DEFAULT);

	const double dpi = GetDPIScaleFactor();
	const wxSize size = GetClientSize();
	const double W = size.x;
	const double H = size.y;
	if (W <= 0 || H <= 0)
		return;

	double tile, gap, anchorX, rowBottom;
	Metrics(tile, gap, anchorX, rowBottom);

	const wxColour white(255, 255, 255);
	const wxColour textDim(170, 178, 200);
	const int n = (int)m_entries.size();
	const double scroll = std::clamp(m_scroll, 0.0, (double)std::max(0, n - 1));
	const int i0 = n ? std::clamp((int)std::floor(scroll), 0, n - 1) : 0;
	const int i1 = n ? std::min(i0 + 1, n - 1) : 0;
	const double t = (n && i1 != i0) ? Clamp01(scroll - i0) : 0.0;

	// ---------- background: game art (crossfaded while scrolling) ----------
	double artAmount = 0.0; // 0 = no art on screen, 1 = art
	{
		if (n)
		{
			bool artA = false, artB = false;
			const wxImage& imgA = GetBackgroundSource(i0, artA);
			const wxImage& imgB = GetBackgroundSource(i1, artB);
			artAmount = Lerp(artA ? 1.0 : 0.0, artB ? 1.0 : 0.0, t);
			const int weight = (int)std::lround(t * 256.0);
			if (!m_bgBitmap.IsOk() || m_bgSrcA != &imgA || m_bgSrcB != &imgB || m_bgWeight != weight)
			{
				wxImage a = imgA;
				if (a.GetWidth() != kBgW || a.GetHeight() != kBgH)
					a = a.Scale(kBgW, kBgH, wxIMAGE_QUALITY_HIGH);
				if (weight > 0 && &imgA != &imgB)
				{
					wxImage b = imgB;
					if (b.GetWidth() != kBgW || b.GetHeight() != kBgH)
						b = b.Scale(kBgW, kBgH, wxIMAGE_QUALITY_HIGH);
					wxImage out(kBgW, kBgH, false);
					const unsigned char* pa = a.GetData();
					const unsigned char* pb = b.GetData();
					unsigned char* po = out.GetData();
					const int wb = weight, wa = 256 - weight;
					const size_t count = (size_t)kBgW * kBgH * 3;
					for (size_t k = 0; k < count; ++k)
						po[k] = (unsigned char)((pa[k] * wa + pb[k] * wb) >> 8);
					m_bgBitmap = wxBitmap(out);
				}
				else
					m_bgBitmap = wxBitmap(a);
				m_bgSrcA = &imgA;
				m_bgSrcB = &imgB;
				m_bgWeight = weight;
			}
			if (m_bgBitmap.IsOk())
			{
				// "cover" fit: fill the window, crop whatever sticks out
				const double scale = std::max(W / kBgW, H / kBgH);
				const double dw = kBgW * scale;
				const double dh = kBgH * scale;
				gc.DrawBitmap(m_bgBitmap, (W - dw) / 2, (H - dh) / 2, dw, dh);
			}
		}
		else
		{
			gc.SetPen(*wxTRANSPARENT_PEN);
			gc.SetBrush(gc.CreateLinearGradientBrush(0, 0, 0, H, wxColour(14, 19, 38), wxColour(4, 5, 11)));
			gc.DrawRectangle(0, 0, W, H);
		}

		// soft darkening so text and tiles stay readable
		gc.SetPen(*wxTRANSPARENT_PEN);
		if (artAmount > 0.0)
		{
			gc.SetBrush(wxBrush(wxColour(0, 0, 0, (unsigned char)std::lround(55.0 * artAmount))));
			gc.DrawRectangle(0, 0, W, H);
			gc.SetBrush(gc.CreateLinearGradientBrush(0, 0, 0, H * 0.22, wxColour(0, 0, 0, (unsigned char)std::lround(120.0 * artAmount)), wxColour(0, 0, 0, 0)));
			gc.DrawRectangle(0, 0, W, H * 0.22);
		}
		gc.SetBrush(gc.CreateLinearGradientBrush(0, H * 0.35, 0, H, wxColour(0, 0, 0, 0), wxColour(0, 0, 0, 200)));
		gc.DrawRectangle(0, 0, W, H);
		gc.SetBrush(gc.CreateLinearGradientBrush(0, 0, W * 0.55, 0, wxColour(0, 0, 0, 120), wxColour(0, 0, 0, 0)));
		gc.DrawRectangle(0, 0, W, H);

		// colour glow in the corner, tinted by the focused game
		if (n)
		{
			const wxColour accent0 = AccentFor(GetCache(i0).accent);
			const wxColour accent1 = AccentFor(GetCache(i1).accent);
			const wxColour glow((unsigned char)Lerp(accent0.Red(), accent1.Red(), t), (unsigned char)Lerp(accent0.Green(), accent1.Green(), t), (unsigned char)Lerp(accent0.Blue(), accent1.Blue(), t), 70);
			gc.SetBrush(gc.CreateRadialGradientBrush(W * 0.86, H * 0.10, W * 0.86, H * 0.10, W * 0.55, glow, WithAlpha(glow, 0.0)));
			gc.DrawRectangle(0, 0, W, H);
		}
	}

	// ---------- top bar ----------
	{
		const wxFont tabFont = GetFont().Scaled(1.55).Bold();
		const double ty = 26.0 * dpi;
		const auto tabRects = TabRects();
		for (int i = 0; i < (int)tabRects.size(); ++i)
		{
			const wxRect2DDouble& r = tabRects[i];
			const bool active = i == m_tab;
			gc.SetFont(tabFont, active ? white : WithAlpha(textDim, 0.75));
			gc.DrawText(TabName(i), r.m_x, ty);
			if (active)
			{
				gc.SetPen(*wxTRANSPARENT_PEN);
				gc.SetBrush(wxBrush(white));
				gc.DrawRoundedRectangle(r.m_x, ty + r.m_height + 6.0 * dpi, r.m_width, 3.0 * dpi, 1.5 * dpi);
			}
		}

		// search box: always there when the library has games (so it can be found and clicked), highlighted while typing or filtering
		if (m_searchActive || !m_searchText.empty() || !m_all.empty())
		{
			const wxFont searchFont = GetFont().Scaled(1.25);
			const wxRect2DDouble searchBox = SearchBoxRect();
			const double boxX = searchBox.m_x, boxY = searchBox.m_y, boxW = searchBox.m_width, boxH = searchBox.m_height;
			const bool searchIdle = !m_searchActive && m_searchText.empty();
			gc.SetPen(wxPen(WithAlpha(white, m_searchActive ? 0.75 : (searchIdle ? 0.18 : 0.30)), std::max(1, (int)std::lround(1.5 * dpi))));
			gc.SetBrush(wxBrush(wxColour(0, 0, 0, 150)));
			gc.DrawRoundedRectangle(boxX, boxY, boxW, boxH, boxH / 2);
			const wxString prompt = wxString::FromUTF8(searchIdle ? "Buscar   /  o  Ctrl+F" : "Buscar:  ");
			wxString shown = m_searchText;
			dc.SetFont(searchFont);
			shown = wxControl::Ellipsize(shown, dc, wxELLIPSIZE_START, (int)(boxW - 40.0 * dpi - 70.0 * dpi));
			gc.SetFont(searchFont, WithAlpha(textDim, searchIdle ? 0.6 : 0.9));
			wxDouble pw, ph;
			gc.GetTextExtent(prompt, &pw, &ph);
			gc.DrawText(prompt, boxX + 18.0 * dpi, boxY + (boxH - ph) / 2.0);
			gc.SetFont(searchFont, white);
			gc.DrawText(shown, boxX + 18.0 * dpi + pw, boxY + (boxH - ph) / 2.0);
			if (m_searchActive)
			{
				wxDouble sw, sh;
				gc.GetTextExtent(shown, &sw, &sh);
				gc.SetPen(*wxTRANSPARENT_PEN);
				gc.SetBrush(wxBrush(white));
				gc.DrawRectangle(boxX + 18.0 * dpi + pw + sw + 2.0 * dpi, boxY + (boxH - ph) / 2.0, std::max(1.0, 1.5 * dpi), ph);
			}
		}
	}

	// ---------- empty library ----------
	if (n == 0)
	{
		const wxFont big = GetFont().Scaled(2.0).Bold();
		const wxFont small = GetFont().Scaled(1.2);
		wxString line1 = wxString::FromUTF8("No se encontraron juegos");
		wxString line2 = wxString::FromUTF8("Haz clic aquí (o pulsa Enter) para elegir tus carpetas de juegos");
		if (!m_searchText.empty())
		{
			line1 = wxString::FromUTF8("Sin resultados");
			line2 = wxString::FromUTF8("Pulsa Esc para borrar la búsqueda");
		}
		else if (!m_all.empty() && m_tab == 0)
		{
			line1 = wxString::FromUTF8("Todos tus juegos están ocultos");
			line2 = wxString::FromUTF8("Abre las opciones (flecha abajo) y activa «Mostrar juegos ocultos»");
		}
		else if (!m_all.empty() && m_tab == 1)
		{
			line1 = wxString::FromUTF8("Todavía no has jugado nada");
			line2 = wxString::FromUTF8("Los juegos que inicies aparecerán aquí");
		}
		else if (!m_all.empty() && m_tab == 2)
		{
			line1 = wxString::FromUTF8("Aún no tienes favoritos");
			line2 = wxString::FromUTF8("Pulsa Espacio sobre un juego para agregarlo");
		}
		else if (!m_all.empty() && m_tab == 3)
		{
			line1 = wxString::FromUTF8("Todavía no has jugado nada");
			line2 = wxString::FromUTF8("Aquí verás los juegos con más tiempo jugado");
		}
		wxDouble w1, h1, w2, h2;
		gc.SetFont(big, white);
		gc.GetTextExtent(line1, &w1, &h1);
		gc.DrawText(line1, (W - w1) / 2, H * 0.40);
		gc.SetFont(small, textDim);
		gc.GetTextExtent(line2, &w2, &h2);
		gc.DrawText(line2, (W - w2) / 2, H * 0.40 + h1 + 10.0 * dpi);
		// without games the options panel still gives access to the Cemu menu (the menu bar is hidden in this view)
		gc.SetFont(GetFont().Scaled(1.1), WithAlpha(textDim, 0.75));
		gc.DrawText(wxString::FromUTF8("\xE2\x86\x93  Men\xC3\xBA de Cemu"), anchorX, H - 36.0 * dpi);
		return;
	}

	// ---------- tiles ----------
	{
		// fade the tiles that scroll away on the left
		for (int pass = 0; pass < 2; ++pass)
		{
			for (int i = 0; i < n; ++i)
			{
				Geom g = GetGeom(i, scroll);
				if (g.x > W || g.x + g.size < -10)
					continue;
				const bool focusedPass = g.closeness > 0.0;
				if ((pass == 0) == focusedPass)
					continue; // first pass: normal tiles, second pass: the focused ones on top

				if (m_fade > 0.0 && g.closeness > 0.5)
				{
					// the started game's tile grows slightly while the rest of the screen fades out
					const double grow = g.size * 0.12 * m_fade;
					g.x -= grow / 2;
					g.y -= grow / 2;
					g.size += grow;
				}

				const double radius = g.size * kCornerRadius;
				const CacheItem& item = GetCache(i);

				// drop shadow
				gc.SetPen(*wxTRANSPARENT_PEN);
				const double shadowStrength = 0.45 + 0.55 * g.closeness;
				for (int k = 0; k < 6; ++k)
				{
					const double spread = k * 5.0 * dpi * (0.6 + 0.6 * g.closeness);
					gc.SetBrush(wxBrush(wxColour(0, 0, 0, (unsigned char)std::lround(26 * shadowStrength))));
					gc.DrawRoundedRectangle(g.x - spread, g.y - spread + 12.0 * dpi * (0.5 + g.closeness), g.size + spread * 2, g.size + spread * 2, radius + spread);
				}

				// colour glow behind the focused tile
				if (g.closeness > 0.01)
				{
					gc.SetBrush(*wxTRANSPARENT_BRUSH);
					for (int k = 0; k < 5; ++k)
					{
						const double expand = (4.0 + k * 5.0) * dpi;
						const int penWidth = std::max(1, (int)std::lround(5.0 * dpi));
						gc.SetPen(wxPen(WithAlpha(AccentFor(item.accent), 0.20 * (1.0 - k / 5.0) * g.closeness), penWidth));
						gc.DrawRoundedRectangle(g.x - expand, g.y - expand, g.size + expand * 2, g.size + expand * 2, radius + expand);
					}
				}

				// the cover itself
				if (item.tile.IsOk())
					gc.DrawBitmap(item.tile, g.x, g.y, g.size, g.size);

				// favorite badge
				if (IsFavorite(m_entries[i].titleId))
				{
					const double bs = g.size * 0.085;
					const double bcx = g.x + g.size - g.size * 0.14;
					const double bcy = g.y + g.size * 0.14;
					gc.SetPen(*wxTRANSPARENT_PEN);
					gc.SetBrush(wxBrush(wxColour(0, 0, 0, 120)));
					gc.DrawEllipse(bcx - bs * 1.5, bcy - bs * 1.5, bs * 3.0, bs * 3.0);
					gc.SetBrush(wxBrush(wxColour(255, 92, 128)));
					gc.FillPath(MakeHeartPath(gc, bcx, bcy, bs));
				}

				// dim the tiles that are not focused
				const double dim = 0.34 * (1.0 - g.closeness);
				if (dim > 0.01)
				{
					gc.SetPen(*wxTRANSPARENT_PEN);
					gc.SetBrush(wxBrush(wxColour(0, 0, 0, (unsigned char)std::lround(dim * 255))));
					gc.DrawRoundedRectangle(g.x, g.y, g.size, g.size, radius);
				}

				// white focus ring
				if (g.closeness > 0.01)
				{
					const double e = 3.0 * dpi;
					gc.SetBrush(*wxTRANSPARENT_BRUSH);
					gc.SetPen(wxPen(WithAlpha(white, g.closeness), std::max(2, (int)std::lround(3.0 * dpi))));
					gc.DrawRoundedRectangle(g.x - e, g.y - e, g.size + e * 2, g.size + e * 2, radius + e);
				}
			}
		}

	}

	// ---------- title, details and start button of the focused game ----------
	{
		const double titleY = rowBottom + 22.0 * dpi;
		const double subtitleY = titleY + 46.0 * dpi;
		const double maxTextWidth = W - anchorX * 1.7;
		const wxFont titleFont = GetFont().Scaled(2.45).Bold();
		const wxFont subtitleFont = GetFont().Scaled(1.2);

		for (int i = i0; i <= i1; ++i)
		{
			const double alpha = Clamp01((GetGeom(i, scroll).closeness - 0.55) / 0.45);
			if (alpha < 0.01)
				continue;
			const wxGameTileEntry& entry = m_entries[i];

			dc.SetFont(titleFont);
			const wxString title = wxControl::Ellipsize(entry.name, dc, wxELLIPSIZE_END, (int)maxTextWidth);
			gc.SetFont(titleFont, WithAlpha(white, alpha));
			gc.DrawText(title, anchorX, titleY);

			wxString details;
			if (!entry.playtime.empty())
				details = wxString::Format(wxString::FromUTF8("Jugado: %s"), entry.playtime);
			if (!entry.lastPlayed.empty())
			{
				if (!details.empty())
					details += wxString::FromUTF8("   \xC2\xB7   ");
				details += wxString::Format(wxString::FromUTF8("Última partida: %s"), entry.lastPlayed);
			}
			if (IsHidden(entry.titleId))
			{
				if (!details.empty())
					details += wxString::FromUTF8("   \xC2\xB7   ");
				details += wxString::FromUTF8("Oculto");
			}
			if (!details.empty())
			{
				gc.SetFont(subtitleFont, WithAlpha(textDim, alpha));
				gc.DrawText(details, anchorX, subtitleY);
			}
		}

		// "Start" pill
		const wxRect2DDouble pill = PillRect();
		gc.SetPen(*wxTRANSPARENT_PEN);
		gc.SetBrush(wxBrush(wxColour(0, 0, 0, 70)));
		gc.DrawRoundedRectangle(pill.m_x - 2.0 * dpi, pill.m_y + 3.0 * dpi, pill.m_width + 4.0 * dpi, pill.m_height + 4.0 * dpi, pill.m_height / 2 + 2.0 * dpi);
		gc.SetBrush(wxBrush(wxColour(255, 255, 255, 245)));
		gc.DrawRoundedRectangle(pill.m_x, pill.m_y, pill.m_width, pill.m_height, pill.m_height / 2);

		// play triangle
		const double cy = pill.m_y + pill.m_height / 2;
		const double tx = pill.m_x + 22.0 * dpi;
		const double ts = 8.0 * dpi;
		wxGraphicsPath tri = gc.CreatePath();
		tri.MoveToPoint(tx, cy - ts);
		tri.AddLineToPoint(tx + ts * 1.5, cy);
		tri.AddLineToPoint(tx, cy + ts);
		tri.CloseSubpath();
		gc.SetBrush(wxBrush(wxColour(14, 18, 32)));
		gc.FillPath(tri);

		const wxFont pillFont = GetFont().Scaled(1.3).Bold();
		gc.SetFont(pillFont, wxColour(14, 18, 32));
		const wxString pillText = wxString::FromUTF8((m_lastLaunched != 0 && GetSelectedTitle() == m_lastLaunched) ? "Continuar" : "Iniciar");
		wxDouble pw, ph;
		gc.GetTextExtent(pillText, &pw, &ph);
		gc.DrawText(pillText, tx + ts * 1.5 + 14.0 * dpi, cy - ph / 2);

		// favorite button
		{
			const wxRect2DDouble hr = HeartRect();
			const bool fav = IsFavorite(GetSelectedTitle());
			const int lineW = std::max(1, (int)std::lround(2.0 * dpi));
			gc.SetPen(wxPen(WithAlpha(white, 0.55), lineW));
			gc.SetBrush(wxBrush(wxColour(255, 255, 255, 38)));
			gc.DrawEllipse(hr.m_x, hr.m_y, hr.m_width, hr.m_height);
			const wxGraphicsPath heart = MakeHeartPath(gc, hr.m_x + hr.m_width / 2, hr.m_y + hr.m_height / 2, hr.m_width * 0.26);
			if (fav)
			{
				gc.SetPen(*wxTRANSPARENT_PEN);
				gc.SetBrush(wxBrush(wxColour(255, 92, 128)));
				gc.FillPath(heart);
			}
			else
			{
				gc.SetBrush(*wxTRANSPARENT_BRUSH);
				gc.SetPen(wxPen(white, lineW));
				gc.StrokePath(heart);
			}
		}
	}

	// ---------- footer ----------
	{
		const wxFont hintFont = GetFont().Scaled(1.1);
		gc.SetFont(hintFont, WithAlpha(textDim, 0.75));
		const wxString hints = m_inputIsPad ? LibraryPadHintText() : wxString::FromUTF8(m_searchActive ? "Escribe para filtrar      \xE2\x86\x90  \xE2\x86\x92  Explorar      Enter  Aceptar      Esc  Borrar búsqueda" : "Enter  Iniciar      \xE2\x86\x90  \xE2\x86\x92  Explorar      Tab  Secciones      Espacio  Favorito      /  Buscar      \xE2\x86\x93  Opciones");
		const double fy = H - 36.0 * dpi;
		gc.DrawText(hints, anchorX, fy);
	}
}

// clock, item counter, options panel, toast and launch fade: what changes while the options panel is open
// (the scene below them can be a cached picture)
void wxGameTileView::RenderTop(wxGraphicsContext& gc, wxDC& dc)
{
	gc.SetInterpolationQuality(wxINTERPOLATION_BEST);
	gc.SetAntialiasMode(wxANTIALIAS_DEFAULT);

	const double dpi = GetDPIScaleFactor();
	const wxSize size = GetClientSize();
	const double W = size.x;
	const double H = size.y;
	if (W <= 0 || H <= 0)
		return;

	double tile, gap, anchorX, rowBottom;
	Metrics(tile, gap, anchorX, rowBottom);
	const wxColour white(255, 255, 255);
	const wxColour textDim(170, 178, 200);
	const int n = (int)m_entries.size();

	// ---------- clock and counter: the options panel slides over this corner, so they fade out with it ----------
	{
		const double fade = 1.0 - Clamp01(m_panelAnim * 2.5); // gone before the panel reaches the corner
		if (fade > 0.01)
		{
			gc.SetFont(GetFont().Scaled(1.45), WithAlpha(white, 0.85 * fade));
			const wxString clock = wxDateTime::Now().Format("%H:%M");
			wxDouble cw, ch;
			gc.GetTextExtent(clock, &cw, &ch);
			gc.DrawText(clock, W - anchorX * 0.7 - cw, 26.0 * dpi);

			if (n > 0)
			{
				gc.SetFont(GetFont().Scaled(1.1), WithAlpha(textDim, 0.75 * fade));
				const wxString counter = wxString::Format("%d / %d", m_selected + 1, n);
				gc.GetTextExtent(counter, &cw, &ch);
				gc.DrawText(counter, W - anchorX * 0.7 - cw, H - 36.0 * dpi);
			}
		}
	}

	// ---------- options panel ----------
	RenderPanel(gc, dc);

	// ---------- toast ----------
	if (m_toastUntilMs > 0)
	{
		const long nowMs = m_frameWatch.Time();
		if (nowMs < m_toastUntilMs)
		{
			const double a = Clamp01((m_toastUntilMs - nowMs) / 300.0);
			const wxFont toastFont = GetFont().Scaled(1.25).Bold();
			gc.SetFont(toastFont, WithAlpha(white, a));
			wxDouble tw, th;
			gc.GetTextExtent(m_toast, &tw, &th);
			const double padX = 22.0 * dpi, padY = 10.0 * dpi;
			const double bx = (W - tw) / 2 - padX;
			const double by = H - 96.0 * dpi - padY;
			gc.SetPen(*wxTRANSPARENT_PEN);
			gc.SetBrush(wxBrush(wxColour(0, 0, 0, (unsigned char)std::lround(175.0 * a))));
			gc.DrawRoundedRectangle(bx, by, tw + padX * 2, th + padY * 2, (th + padY * 2) / 2);
			gc.DrawText(m_toast, bx + padX, by + padY);
		}
	}

	// ---------- launch transition: fade everything to black ----------
	if (m_fade > 0.0)
	{
		gc.SetPen(*wxTRANSPARENT_PEN);
		gc.SetBrush(wxBrush(wxColour(0, 0, 0, (unsigned char)std::lround(Clamp01(m_fade) * 255.0))));
		gc.DrawRectangle(0, 0, W, H);
	}
}
