#pragma once

#include "config/XMLConfig.h"
#include "util/math/vector2.h"

#include "wxgui.h"

namespace DefaultColumnSize
{
	enum : uint32
	{
		name = 500u,
		version = 60u,
		dlc = 50u,
		game_time = 140u,
		game_started = 160u,
		region = 80u,
		title_id = 160u
	};
};

enum class MSWThemeOption : int
{
	kAuto = 0,
	kLight = 1,
	kDark = 2,
};
ENABLE_ENUM_ITERATORS(MSWThemeOption, MSWThemeOption::kAuto, MSWThemeOption::kDark);

typedef union
{
	struct
	{
		uint16 key : 13; // enough bits for all keycodes
		uint16 alt : 1;
		uint16 ctrl : 1;
		uint16 shift : 1;
	};
	uint16 raw;
} uKeyboardHotkey;

typedef sint16 ControllerHotkey_t;

struct sHotkeyCfg
{
	static constexpr uint8 keyboardNone{WXK_NONE};
	static constexpr sint8 controllerNone{-1}; // no enums to work with, but buttons start from 0

	uKeyboardHotkey keyboard{keyboardNone};
	ControllerHotkey_t controller{controllerNone};

	/* for defaults */
	sHotkeyCfg(const uKeyboardHotkey& keyboard = {keyboardNone}, const ControllerHotkey_t& controller = {controllerNone}) : keyboard(keyboard), controller(controller) {};

	/* for reading from xml */
	sHotkeyCfg(const char* xml_values)
	{
		std::istringstream iss(xml_values);
		iss >> keyboard.raw >> controller;
	}
};

template<>
struct fmt::formatter<sHotkeyCfg> : formatter<string_view>
{
	template<typename FormatContext>
	auto format(const sHotkeyCfg c, FormatContext& ctx) const
	{
		std::string xml_values = fmt::format("{} {}", c.keyboard.raw, c.controller);
		return formatter<string_view>::format(xml_values, ctx);
	}
};

struct wxCemuConfig
{
	ConfigValue<sint32> language{wxLANGUAGE_SPANISH}; // Spanish unless the user picked another language
	ConfigValue<int> msw_theme { static_cast<int>(MSWThemeOption::kDark) }; // dark by default, like the rest of this interface (a saved choice is kept)
	ConfigValue<bool> use_discord_presence{true};
	ConfigValue<bool> fullscreen{ false };
	ConfigValue<bool> borderless_fullscreen{ false }; // borderless window over the usable area of the monitor (only used when fullscreen is off)
	ConfigValue<bool> fullscreen_menubar{false};
	ConfigValue<bool> feral_gamemode{false};

	// max 15 entries
	static constexpr size_t kMaxRecentEntries = 15;
	std::vector<std::string> recent_launch_files;
	std::vector<std::string> recent_nfc_files;

	Vector2i window_position{0, 0}; // (-1,-1) = remember window disabled, (0,0) = enabled but nothing stored yet
	Vector2i window_size{0, 0};
	ConfigValue<bool> window_maximized;

	ConfigValue<bool> pad_open;
	Vector2i pad_position{-1, -1};
	Vector2i pad_size{-1, -1};
	ConfigValue<bool> pad_maximized;

	ConfigValue<bool> check_update{true};
	ConfigValue<bool> receive_untested_updates{false};
	ConfigValue<bool> save_screenshot{true};

	ConfigValue<bool> did_show_vulkan_warning{false};
	ConfigValue<bool> did_show_graphic_pack_download{false}; // no longer used but we keep the config value around in case people downgrade Cemu. Despite the name this was used for the Getting Started dialog
	ConfigValue<bool> did_show_macos_disclaimer{false}; // no longer used

	ConfigValue<bool> show_icon_column{true};

	int game_list_style = 3; // 0 list, 1 icons, 2 small icons, 3 tiles
	std::string game_list_column_order;
	int tile_size = 1;                 // size of the cards: 0 small, 1 normal, 2 large
	int tile_accent = 0;               // accent colour of the glow: 0 from the game, 1..6 fixed colours
	int tile_sort = 0;                 // order of the Juegos / Favoritos tabs: 0 name, 1 play time, 2 last played
	ConfigValue<bool> tile_volume_done{false}; // the TV volume was set to 100% once (first run of this interface)
	int tile_button_style = 0;         // buttons printed in the hints: 0 auto, 1 PlayStation, 2 Xbox, 3 Nintendo
	int tile_last_tab = 0;             // what the library remembers: section, focused game and last launched game
	std::string tile_last_selected;    // title ids as 16 hex digits, empty = none
	std::string tile_last_launched;
	std::string tile_favorites; // title ids (16 hex digits) separated by commas, favorites of the tile view
	std::string tile_hidden;    // same format: games the user hid from the tile view
	ConfigValue<bool> tile_show_hidden{false}; // show the hidden games again (marked as hidden)
	ConfigValue<bool> tile_sound{true};
	struct
	{
		uint32 name = DefaultColumnSize::name;
		uint32 version = DefaultColumnSize::version;
		uint32 dlc = DefaultColumnSize::dlc;
		uint32 game_time = DefaultColumnSize::game_time;
		uint32 game_started = DefaultColumnSize::game_started;
		uint32 region = DefaultColumnSize::region;
		uint32 title_id = 0;
	} column_width{};

	// hotkeys
	struct
	{
		sHotkeyCfg modifiers;
		sHotkeyCfg toggleFullscreen;
		sHotkeyCfg toggleFullscreenAlt;
		sHotkeyCfg openGameMenu; // in-game menu (pauses the game)
		sHotkeyCfg exitFullscreen;
		sHotkeyCfg takeScreenshot;
		sHotkeyCfg toggleFastForward;
		sHotkeyCfg exitApplication;
#ifdef CEMU_DEBUG_ASSERT
		sHotkeyCfg endEmulation;
#endif
	} hotkeys{};

	std::vector<uint64_t> GetTileFavorites() const;
	std::vector<uint64_t> GetTileHidden() const;
	void SetTileHidden(const std::vector<uint64_t>& titleIds);
	void SetTileFavorites(const std::vector<uint64_t>& titleIds);
	void GetTileState(int& tab, uint64_t& selected, uint64_t& launched) const;
	void SetTileState(int tab, uint64_t selected, uint64_t launched);

	void AddRecentlyLaunchedFile(std::string_view file);
	void AddRecentNfcFile(std::string_view file);

	void Load(XMLConfigParser& parser);
	void Save(XMLConfigParser& parser);
};

typedef XMLChildConfig<wxCemuConfig, &wxCemuConfig::Load, &wxCemuConfig::Save> XMLWxCemuConfig_t;

extern XMLWxCemuConfig_t g_wxConfig;

inline XMLWxCemuConfig_t& GetWxGuiConfigHandle()
{
	return g_wxConfig;
}

inline wxCemuConfig& GetWxGUIConfig()
{
	return GetWxGuiConfigHandle().Data();
}
