#pragma once

#include <wx/window.h>
#include <wx/geometry.h>
#include <wx/bitmap.h>
#include <wx/image.h>
#include <wx/colour.h>
#include <wx/string.h>
#include <wx/timer.h>
#include <wx/stopwatch.h>
#include <wx/sound.h>

#include "wxgui/components/wxGamepadState.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// One game as shown in the tile view
struct wxGameTileEntry
{
	uint64_t titleId{};
	uint64_t iconKey{}; // changes whenever the icon changes (used to invalidate cached renders)
	wxString name;
	wxBitmap icon;
	std::shared_ptr<const wxImage> art; // 16:9 game art (bootTvTex), null if the game has none
	wxString playtime;   // already localized, empty if unknown
	wxString lastPlayed; // already localized, empty if unknown
	uint32_t lastPlayedKey{}; // yyyymmdd of the last session, 0 = never played (used for the "Recientes" tab)
	uint32_t minutesPlayed{}; // total play time in minutes (used for the "Más jugados" tab and the sort order)
};

// What the library remembers between runs (stored by the owner in the Cemu config)
struct wxGameTileState
{
	int tab{};              // 0 = Juegos, 1 = Recientes, 2 = Favoritos
	uint64_t selected{};    // game that was focused
	uint64_t launched{};    // last game started from the library ("Continuar")
};

// One row of the options panel supplied by the owner (id 0 = section header)
struct wxGameTileOption
{
	int id{};
	wxString label;
	bool enabled{true};
};

// Details page of a game (supplied by the owner; the play statistics come from the entry itself)
struct wxGameTileDetailRow
{
	wxString label;
	wxString value;
	bool copy{false}; // accepting the row copies its value to the clipboard
};

struct wxGameTileDetails
{
	std::vector<wxGameTileDetailRow> info;  // title id, region, versions...
	std::vector<wxGameTileDetailRow> packs; // graphic packs enabled for the game (label = pack, value = active preset)
	std::vector<wxGameTileOption> actions;  // shortcuts (game profile, graphic packs), run through onOption
};

// One entry of the Cemu menu shown as pages of the options panel (mirrors the classic menu bar)
struct wxGameTileMenuItem
{
	int id{};
	wxString label;
	bool enabled{true};
	bool separator{false};
	bool submenu{false};   // opens a page with the entries in children
	bool checkable{false}; // check or radio entry
	bool radio{false};
	bool checked{false};
	std::vector<wxGameTileMenuItem> children;
};

// Console-style game "home" screen: one big carousel of rounded cover tiles with an
// ambient background tinted by the selected game, smooth animation and keyboard/mouse support.
// This window is purely a view. All data comes from (and all actions go to) the callbacks below.
class wxGameTileView : public wxWindow
{
public:
	wxGameTileView(wxWindow* parent, wxWindowID id = wxID_ANY);
	~wxGameTileView() override;

	// returns a value that changes whenever the entries (or their icons / names) change. Must be cheap.
	std::function<uint64_t()> onQueryRevision;
	// returns the full entry list in display order
	std::function<std::vector<wxGameTileEntry>()> onFetchEntries;
	std::function<void(uint64_t)> onLaunch;
	std::function<void(uint64_t)> onContextMenu;
	std::function<void()> onOpenSettings; // used when the library is empty
	// options panel: rows supplied by the owner for a game, and the callback when one of them is chosen
	std::function<std::vector<wxGameTileOption>(uint64_t)> onFetchOptions;
	std::function<void(uint64_t, int)> onOption;
	std::function<wxGameTileDetails(uint64_t)> onFetchDetails; // "Detalles del juego" page (the row only exists when this is set)
	// "Menú de Cemu" pages of the options panel: top level menus (each one a submenu entry) and the callback to run an entry
	std::function<std::vector<wxGameTileMenuItem>()> onFetchMenu;
	std::function<void(int)> onMenuCommand;
	// in-game overlay mode (see wxGameMenuOverlay): the view only draws the options panel, filling its whole window
	std::function<void()> onPanelClosed;    // the panel was closed from its first page
	std::function<void()> onBackToLibrary;  // "Volver a la biblioteca" row (the row only exists when this is set); called after the confirmation page
	std::function<void()> onQuit;           // "Salir de Cemu" row (the row only exists when this is set); called after the confirmation page
	std::function<int()> onGetDisplayMode;  // overlay: 0 = window, 1 = fullscreen, 2 = fullscreen without border
	std::function<void(int)> onSetDisplayMode; // overlay: the "Pantalla" rows only exist when this is set
	// remembered tab / focused game / last launched game
	std::function<wxGameTileState()> onLoadState;
	std::function<void(const wxGameTileState&)> onSaveState;
	// in-game overlay: quick actions (the rows only exist when the callbacks are set)
	std::function<void()> onScreenshot;
	std::function<bool()> onGetFastForward;
	std::function<void(bool)> onSetFastForward;
	std::function<int()> onGetVolume;       // 0-100
	std::function<void(int)> onSetVolume;
	// which buttons the hints show: 0 auto, 1 PlayStation, 2 Xbox, 3 Nintendo (stored by the owner)
	std::function<int()> onGetButtonStyle;
	std::function<void(int)> onSetButtonStyle;
	// look of the library: card size 0 small / 1 normal / 2 large, accent colour 0 from the game / 1..6 fixed (stored by the owner)
	std::function<int()> onGetTileSize;
	std::function<void(int)> onSetTileSize;
	std::function<int()> onGetAccent;
	std::function<void(int)> onSetAccent;
	// order of the games list: 0 name, 1 play time, 2 last played (stored by the owner)
	std::function<int()> onGetSortMode;
	std::function<void(int)> onSetSortMode;
	// shown when a game is started: the owner can put its loading splash on screen (and take it away if the launch fails)
	std::function<void(const wxString&, const wxBitmap&, const std::shared_ptr<const wxImage>&)> onLaunchSplash;
	std::function<void()> onLaunchSplashCancel;
	// favorites and the sound switch are stored by the owner (Cemu config)
	std::function<std::vector<uint64_t>()> onLoadFavorites;
	std::function<void(const std::vector<uint64_t>&)> onSaveFavorites;
	// hidden games (stored by the owner) and the switch that shows them again
	std::function<std::vector<uint64_t>()> onLoadHidden;
	std::function<void(const std::vector<uint64_t>&)> onSaveHidden;
	std::function<bool()> onLoadShowHidden;
	std::function<void(bool)> onSaveShowHidden;
	std::function<bool()> onLoadSoundEnabled;
	std::function<void(bool)> onSaveSoundEnabled;

	enum class NavAction { Up, Down, Left, Right, Accept, Back };
	void SetOverlayMode(bool overlay) { m_overlayMode = overlay; }
	void OpenOverlayPanel();           // overlay mode: opens the first page of the panel
	void Navigate(NavAction action);   // gamepad navigation of the panel

	void ReloadEntries();
	void SelectTitle(uint64_t titleId);
	void SetSoundEnabled(bool enabled) { m_soundEnabled = enabled; }
	uint64_t GetSelectedTitle() const;

private:
	struct CacheItem
	{
		uint64_t iconKey{};
		wxBitmap tile;    // rounded, with alpha
		wxColour accent;
		wxImage fallbackBg; // art-sized colour wash, only built for games without art
	};

	struct Geom
	{
		double x, y, size, closeness;
	};

	void OnPaint(wxPaintEvent& event);
	void Render(class wxGraphicsContext& gc, class wxDC& dc);
	void OnSize(wxSizeEvent& event);
	void OnKeyDown(wxKeyEvent& event);
	void OnChar(wxKeyEvent& event);
	void OnMouseDown(wxMouseEvent& event);
	void OnMouseDClick(wxMouseEvent& event);
	void OnMouseRightUp(wxMouseEvent& event);
	void OnMouseWheel(wxMouseEvent& event);
	void OnMouseMove(wxMouseEvent& event);
	void OnShow(wxShowEvent& event);
	void OnTimer(wxTimerEvent& event);

	void Metrics(double& tile, double& gap, double& anchorX, double& rowBottom) const;
	Geom GetGeom(int index, double scroll) const;
	int HitTestTile(const wxPoint& pos) const;
	wxRect2DDouble PillRect() const;

	void SetSelected(int index, bool playSound = true);
	void MoveSelection(int delta);
	void LaunchSelected();
	void StartAnimation();
	void BuildSounds();
	void PlayTick();
	void PlayLaunchSound();
	void ResetLaunchState();

	// sections (tabs), favorites, toast
	void RebuildEntries(uint64_t keepTitle, bool snapScroll);
	void SetTab(int tab);
	bool IsFavorite(uint64_t titleId) const;
	void ToggleFavorite();
	void ToggleSound();
	void ShowToast(const wxString& text);
	std::vector<wxRect2DDouble> TabRects() const;
	int HitTestTab(const wxPoint& pos) const;
	wxRect2DDouble HeartRect() const;
	wxRect2DDouble SearchBoxRect() const;
	CacheItem& GetCache(int index);

	// options panel (slides in from the right, PS5 style)
	struct PanelItem
	{
		int id;
		wxString label;
		bool enabled;
		int sub = -1;            // menu pages: index of the entry inside its menu level
		int cmd = 0;             // menu pages: command id to run
		bool checkable = false;
		bool radio = false;
		bool checked = false;
		bool note = false;       // plain text row of a page (not selectable, id 0)
		wxString value{};        // details page: text shown on the right of the row
		bool copyOnAccept = false;
		wxString shownValue{};   // value after cutting it to the available width
		wxString shownValueKey{};
		int shownValueW = -1;
		int valueTextW = 0;
		// text layout cache used by RenderPanel: the label after cutting it to the available width (measuring text is slow)
		wxString shownKey{};
		wxString shown{};
		int shownW = -1;
		int shownMode = 0;
	};
	static constexpr int kPanelStart = -1;
	static constexpr int kPanelFavorite = -2;
	static constexpr int kPanelSound = -3;
	static constexpr int kPanelSettings = -4;
	static constexpr int kPanelMore = -5;
	static constexpr int kPanelCemuMenu = -6;    // opens the "Menú de Cemu" pages
	static constexpr int kPanelSubmenu = -7;     // menu page entry that opens another page
	static constexpr int kPanelMenuCommand = -8; // menu page entry that runs a command
	static constexpr int kPanelResume = -9;      // overlay: close the panel and continue the game
	static constexpr int kPanelBackToLibrary = -10; // overlay: back to the library
	static constexpr int kPanelFullscreen = -11;    // overlay: fullscreen (check)
	static constexpr int kPanelBorderless = -12;    // overlay: fullscreen without border (check)
	static constexpr int kPanelScreenshot = -13;    // overlay: take a screenshot
	static constexpr int kPanelFastForward = -14;   // overlay: fast-forward (check)
	static constexpr int kPanelVolume = -15;        // overlay: volume (left / right or accept change it)
	static constexpr int kPanelButtons = -16;       // button layout of the hints (cycles on accept)
	static constexpr int kPanelSort = -17;          // order of the games (cycles on accept)
	static constexpr int kPanelQuit = -18;          // quit Cemu (opens the confirmation page)
	static constexpr int kPanelConfirmNo = -19;     // confirmation page: cancel
	static constexpr int kPanelConfirmYes = -20;    // confirmation page: do it
	static constexpr int kPanelTileSize = -23;      // card size (cycles on accept)
	static constexpr int kPanelAccent = -24;        // accent colour (cycles on accept)
	int m_tileSize = 1;
	int m_accentMode = 0;
	wxColour AccentFor(const wxColour& fromGame) const;
	static constexpr int kPanelHide = -21;          // hide / unhide the focused game
	static constexpr int kPanelShowHidden = -22;    // show or not the hidden games (switch)
	bool IsHidden(uint64_t titleId) const;
	void ToggleHidden();
	void ToggleShowHidden();
	void OpenSearch();
	void CloseSearch(bool clear);
	void ApplySearch();
	static constexpr int kConfirmBackToLibrary = 1; // values of m_panelConfirm
	static constexpr int kConfirmQuit = 2;
	static constexpr int kPageDetails = 3;          // also a value of m_panelConfirm: the details page of the game (not a confirmation)
	static constexpr int kPanelDetails = -25;       // opens the details page of the focused game
	static constexpr int kPanelDetail = -26;        // a row of the details page
	void OpenConfirmPage(int what, int returnSel);
	void OpenDetailsPage(int returnSel);
	int GetSortMode() const;
	void SortEntries(std::vector<wxGameTileEntry>& entries) const;
	int ResolveButtonStyle();                       // 0 PlayStation, 1 Xbox, 2 Nintendo
	wxString PanelHintText();
	wxString LibraryPadHintText();
	void SetInputIsPad(bool pad);
	void AdjustPanelVolume(int delta);
	bool PanelVolumeSelected() const;
	void MarkStateDirty();
	void SaveState(bool force);
	void UpdateCursorHide();
	void RefreshRootPage();   // rebuilds the first page keeping the selected row (a check row just changed)
	void PollGamepad();       // library: navigation with the controller
	void OpenPanel();
	void BuildPanelRoot();
	void BuildMenuPage();
	void FinishPanelPage(int preferredSel);
	void RefreshMenuPage();
	void PanelBack();
	const std::vector<wxGameTileMenuItem>* PanelMenuLevel() const;
	void ClosePanel();
	void ActivatePanelItem(int index);
	void HandlePanelKey(wxKeyEvent& event);
	void MovePanelSelection(int dir);
	void SetPanelSelection(int index);
	int PanelRowAt(const wxPoint& pos) const;
	bool PanelContains(const wxPoint& pos) const;
	void PanelMetrics(double& x0, double& width, double& listTop, double& listHeight) const;
	double PanelRowHeight(const PanelItem& item) const;
	std::vector<double> PanelOffsets(double& total) const;
	void PanelTargets(double& scrollTarget, double& highlightTarget) const;
	void RenderPanel(class wxGraphicsContext& gc, class wxDC& dc);
	void RenderScene(class wxGraphicsContext& gc, class wxDC& dc); // background, tabs, tiles, title, footer
	void RenderTop(class wxGraphicsContext& gc, class wxDC& dc);   // clock, counter, options panel, toast, launch fade
	void CaptureSceneSnapshot(class wxDC& source, const wxSize& size);
	void InvalidateScene();
	wxRect PanelDirtyRect() const;
	const wxImage& GetBackgroundSource(int index, bool& hasArt);

	std::vector<wxGameTileEntry> m_all;     // every game, in list order
	std::vector<wxGameTileEntry> m_entries; // what the current tab shows
	std::vector<uint64_t> m_favorites;
	std::vector<uint64_t> m_hidden;
	bool m_showHidden = false;
	bool m_searchActive = false; // typing in the search box
	wxString m_searchText;       // the filter (stays after Enter, Esc clears it)
	int m_tab = 0; // 0 = Juegos, 1 = Recientes, 2 = Favoritos
	wxString m_toast;
	long m_toastUntilMs = 0;
	std::unordered_map<uint64_t, CacheItem> m_cache;
	std::vector<PanelItem> m_panelItems;
	bool m_panelOpen = false;
	double m_panelAnim = 0.0;    // 0 = hidden, 1 = fully open
	uint64_t m_panelTitle = 0;
	int m_panelSel = -1;
	double m_panelHiY = 0.0;     // animated highlight position (content coordinates)
	double m_panelScroll = 0.0;
	bool m_overlayMode = false;                  // in-game overlay: no carousel, the panel fills the whole window
	// while the options panel is open the carousel behind it does not change: it is drawn once into this picture and reused
	wxBitmap m_sceneSnapshot;
	bool m_sceneValid = false;
	// cached fonts and text layout of the options panel (rebuilt only when the dpi, the width or the text change)
	double m_panelFontDpi = -1.0;
	wxFont m_panelTitleFont, m_panelSubFont, m_panelRowFont, m_panelHeaderFont;
	double m_panelHeadLineH = 0.0, m_panelHeadSubH = 0.0, m_panelRowTextH = 0.0, m_panelHeaderTextH = 0.0;
	wxString m_panelHeadKey;
	int m_panelHeadW = -1;
	std::vector<wxString> m_panelHeadLines;
	int m_panelConfirm = 0;                      // 0 = none, else the confirmation page that is shown (kConfirm...)
	int m_panelConfirmReturnSel = -1;            // row of the first page to go back to when the confirmation is cancelled
	bool m_panelMenuMode = false;                // showing the "Menú de Cemu" pages instead of the game options
	std::vector<wxGameTileMenuItem> m_menuTree;  // top level menus, fetched when the pages are entered
	std::vector<int> m_panelPath;                // indices of the opened submenus inside m_menuTree
	std::vector<int> m_panelSelStack;            // selected row of each page left behind, to restore it when going back
	wxString m_panelHeadTitle;                   // menu pages: title and subtitle of the panel header
	wxString m_panelSubtitle;
	// blended background (crossfade between two games), rebuilt only when the inputs change
	wxBitmap m_bgBitmap;
	int m_bgWeight = -1;
	const wxImage* m_bgSrcA = nullptr;
	const wxImage* m_bgSrcB = nullptr;
	int m_selected = 0;
	double m_scroll = 0.0;
	uint64_t m_revision = ~0ull;
	int m_wheelAccum = 0;

	// remembered state (tab, focused game, last launched game), saved a moment after it stops changing
	bool m_inputIsPad = false;   // the last thing used was the controller: the hints show its buttons
	int m_autoStyle = 0;
	long m_autoStyleMs = -100000;
	bool m_stateLoaded = false;
	bool m_stateDirty = false;
	long m_stateDirtyMs = 0;
	uint64_t m_pendingTitle = 0;  // game to focus once the list has it
	uint64_t m_lastLaunched = 0;

	// the mouse cursor hides itself after a few seconds without moving
	wxPoint m_lastMousePos{-1, -1};
	long m_lastMouseMs = 0;
	bool m_cursorHidden = false;

	wxTimer m_animTimer;
	wxTimer m_pollTimer;
	wxTimer m_padTimer;                 // controller polling of the library
	wxGamepadState m_padPrev;
	bool m_padPrimed = false;        // the first poll only records which buttons are already held
	long m_padNextUp = 0, m_padNextDown = 0, m_padNextLeft = 0, m_padNextRight = 0; // auto repeat
	wxStopWatch m_frameWatch;
	long m_lastFrameMs = 0;

	// launch transition (menu fades to black before the game starts)
	bool m_launching = false;
	bool m_launchCalled = false;
	uint64_t m_launchTitle = 0;
	long m_launchStartMs = 0;
	long m_launchCalledMs = 0;
	double m_fade = 0.0; // 0 = normal, 1 = fully black

	// menu sounds (synthesized at startup, no external files)
	bool m_soundEnabled = true;
	std::shared_ptr<const std::vector<unsigned char>> m_tickData;   // shared with the sound worker thread (Windows), which may still be playing them
	std::shared_ptr<const std::vector<unsigned char>> m_launchData;
#if wxUSE_SOUND && !defined(__WXMSW__)
	wxSound m_tickSound;
	wxSound m_launchSound;
#endif
};
