#include "wxCemuConfig.h"
#include "Common/precompiled.h"
#include "config/CemuConfig.h"
#include "config/XMLConfig.h"
#include "util/helpers/helpers.h"
#include <wx/language.h>
#include <algorithm>

XMLWxCemuConfig_t g_wxConfig(&GetConfigHandle);

std::vector<uint64_t> wxCemuConfig::GetTileFavorites() const
{
	std::vector<uint64_t> result;
	size_t pos = 0;
	while (pos < tile_favorites.size())
	{
		size_t end = tile_favorites.find(',', pos);
		if (end == std::string::npos)
			end = tile_favorites.size();
		if (end > pos)
		{
			const std::string token = tile_favorites.substr(pos, end - pos);
			char* endPtr = nullptr;
			const uint64_t value = std::strtoull(token.c_str(), &endPtr, 16);
			if (endPtr != token.c_str() && value != 0)
				result.push_back(value);
		}
		pos = end + 1;
	}
	return result;
}

void wxCemuConfig::SetTileFavorites(const std::vector<uint64_t>& titleIds)
{
	std::string text;
	for (const uint64_t titleId : titleIds)
	{
		if (!text.empty())
			text += ',';
		text += fmt::format("{:016x}", titleId);
	}
	tile_favorites = text;
}

std::vector<uint64_t> wxCemuConfig::GetTileHidden() const
{
	std::vector<uint64_t> result;
	size_t pos = 0;
	while (pos < tile_hidden.size())
	{
		size_t end = tile_hidden.find(',', pos);
		if (end == std::string::npos)
			end = tile_hidden.size();
		if (end > pos)
		{
			const std::string token = tile_hidden.substr(pos, end - pos);
			char* endPtr = nullptr;
			const uint64_t value = std::strtoull(token.c_str(), &endPtr, 16);
			if (endPtr != token.c_str() && value != 0)
				result.push_back(value);
		}
		pos = end + 1;
	}
	return result;
}

void wxCemuConfig::SetTileHidden(const std::vector<uint64_t>& titleIds)
{
	std::string text;
	for (const uint64_t titleId : titleIds)
	{
		if (!text.empty())
			text += ',';
		text += fmt::format("{:016x}", titleId);
	}
	tile_hidden = text;
}

void wxCemuConfig::GetTileState(int& tab, uint64_t& selected, uint64_t& launched) const
{
	tab = tile_last_tab;
	selected = tile_last_selected.empty() ? 0 : std::strtoull(tile_last_selected.c_str(), nullptr, 16);
	launched = tile_last_launched.empty() ? 0 : std::strtoull(tile_last_launched.c_str(), nullptr, 16);
}

void wxCemuConfig::SetTileState(int tab, uint64_t selected, uint64_t launched)
{
	tile_last_tab = tab;
	tile_last_selected = selected ? fmt::format("{:016x}", selected) : std::string();
	tile_last_launched = launched ? fmt::format("{:016x}", launched) : std::string();
}

void wxCemuConfig::AddRecentlyLaunchedFile(std::string_view file)
{
	recent_launch_files.insert(recent_launch_files.begin(), std::string(file));
	RemoveDuplicatesKeepOrder(recent_launch_files);
	while (recent_launch_files.size() > kMaxRecentEntries)
		recent_launch_files.pop_back();
}

void wxCemuConfig::AddRecentNfcFile(std::string_view file)
{
	recent_nfc_files.insert(recent_nfc_files.begin(), std::string(file));
	RemoveDuplicatesKeepOrder(recent_nfc_files);
	while (recent_nfc_files.size() > kMaxRecentEntries)
		recent_nfc_files.pop_back();
}

void wxCemuConfig::Load(XMLConfigParser& parser)
{
	language = parser.get<sint32>("language", wxLANGUAGE_SPANISH);
	msw_theme = parser.get<sint32>("msw_theme", msw_theme);
	use_discord_presence = parser.get("use_discord_presence", true);
	fullscreen_menubar = parser.get("fullscreen_menubar", false);
	feral_gamemode = parser.get("feral_gamemode", false);
	check_update = parser.get("check_update", check_update);
	receive_untested_updates = parser.get("receive_untested_updates", receive_untested_updates);
	save_screenshot = parser.get("save_screenshot", save_screenshot);
	did_show_vulkan_warning = parser.get("vk_warning", did_show_vulkan_warning);
	did_show_graphic_pack_download = parser.get("gp_download", did_show_graphic_pack_download);
	did_show_macos_disclaimer = parser.get("macos_disclaimer", did_show_macos_disclaimer);
	fullscreen = parser.get("fullscreen", fullscreen);
	borderless_fullscreen = parser.get("borderless_fullscreen", borderless_fullscreen);

	window_position.x = parser.get("window_position").get("x", 0);
	window_position.y = parser.get("window_position").get("y", 0);

	window_size.x = parser.get("window_size").get("x", 0);
	window_size.y = parser.get("window_size").get("y", 0);
	window_maximized = parser.get("window_maximized", false);

	pad_open = parser.get("open_pad", false);
	pad_position.x = parser.get("pad_position").get("x", -1);
	pad_position.y = parser.get("pad_position").get("y", -1);

	pad_size.x = parser.get("pad_size").get("x", -1);
	pad_size.y = parser.get("pad_size").get("y", -1);
	pad_maximized = parser.get("pad_maximized", false);

	auto gamelist = parser.get("GameList");
	game_list_style = gamelist.get("style", 3);
	game_list_column_order = gamelist.get("order", "");

	show_icon_column = parser.get("show_icon_column", true);
	tile_favorites = parser.get("tile_favorites", "");
	tile_hidden = parser.get("tile_hidden", "");
	tile_show_hidden = parser.get("tile_show_hidden", false);
	tile_last_tab = parser.get<sint32>("tile_last_tab", 0);
	tile_button_style = parser.get<sint32>("tile_button_style", 0);
	tile_sort = std::clamp<sint32>(parser.get<sint32>("tile_sort", 0), 0, 2);
	tile_size = std::clamp<sint32>(parser.get<sint32>("tile_size", 1), 0, 2);
	tile_accent = std::clamp<sint32>(parser.get<sint32>("tile_accent", 0), 0, 6);
	tile_volume_done = parser.get("tile_volume_done", false);
	tile_last_selected = parser.get("tile_last_selected", "");
	tile_last_launched = parser.get("tile_last_launched", "");
	tile_sound = parser.get("tile_sound", true);

	// return default width if value in config file out of range
	auto loadColumnSize = [&gamelist](const char* name, uint32 defaultWidth) {
		sint64 val = gamelist.get(name, defaultWidth);
		if (val < 0 || val > (sint64)std::numeric_limits<uint32>::max)
			return defaultWidth;
		return static_cast<uint32>(val);
	};
	column_width.name = loadColumnSize("name_width", DefaultColumnSize::name);
	column_width.version = loadColumnSize("version_width", DefaultColumnSize::version);
	column_width.dlc = loadColumnSize("dlc_width", DefaultColumnSize::dlc);
	column_width.game_time = loadColumnSize("game_time_width", DefaultColumnSize::game_time);
	column_width.game_started = loadColumnSize("game_started_width", DefaultColumnSize::game_started);
	column_width.region = loadColumnSize("region_width", DefaultColumnSize::region);
	column_width.title_id = loadColumnSize("title_id", DefaultColumnSize::title_id);

	recent_launch_files.clear();
	auto launch_parser = parser.get("RecentLaunchFiles");
	for (auto element = launch_parser.get("Entry"); element.valid(); element = launch_parser.get("Entry", element))
	{
		const std::string path = element.value("");
		if (path.empty())
			continue;

		try
		{
			recent_launch_files.emplace_back(path);
		} catch (const std::exception&)
		{
			cemuLog_log(LogType::Force, "config load error: can't load recently launched game file: {}", path);
		}
	}

	recent_nfc_files.clear();
	auto nfc_parser = parser.get("RecentNFCFiles");
	for (auto element = nfc_parser.get("Entry"); element.valid(); element = nfc_parser.get("Entry", element))
	{
		const std::string path = element.value("");
		if (path.empty())
			continue;
		try
		{
			recent_nfc_files.emplace_back(path);
		} catch (const std::exception&)
		{
			cemuLog_log(LogType::Force, "config load error: can't load recently launched nfc file: {}", path);
		}
	}

	// hotkeys
	auto xml_hotkeys = parser.get("Hotkeys");
	hotkeys.modifiers = xml_hotkeys.get("modifiers", sHotkeyCfg{});
	hotkeys.exitFullscreen = xml_hotkeys.get("ExitFullscreen", sHotkeyCfg{uKeyboardHotkey{WXK_ESCAPE}});
	hotkeys.toggleFullscreen = xml_hotkeys.get("ToggleFullscreen", sHotkeyCfg{uKeyboardHotkey{WXK_F11}});
	hotkeys.toggleFullscreenAlt = xml_hotkeys.get("ToggleFullscreenAlt", sHotkeyCfg{uKeyboardHotkey{WXK_CONTROL_M, true}}); // ALT+ENTER
	hotkeys.openGameMenu = xml_hotkeys.get("OpenGameMenu", sHotkeyCfg{uKeyboardHotkey{WXK_F1}});
	hotkeys.takeScreenshot = xml_hotkeys.get("TakeScreenshot", sHotkeyCfg{uKeyboardHotkey{WXK_F12}});
	hotkeys.toggleFastForward = xml_hotkeys.get("ToggleFastForward", sHotkeyCfg{});
	hotkeys.exitApplication = xml_hotkeys.get("ExitApplication", sHotkeyCfg{});
#ifdef CEMU_DEBUG_ASSERT
	hotkeys.endEmulation = xml_hotkeys.get("EndEmulation", sHotkeyCfg{uKeyboardHotkey{WXK_F5}});
#endif
}

void wxCemuConfig::Save(XMLConfigParser& config)
{
	// general settings
	config.set<sint32>("language", language);
	config.set<sint32>("msw_theme", msw_theme);
	config.set<bool>("use_discord_presence", use_discord_presence);
	config.set<bool>("fullscreen_menubar", fullscreen_menubar);
	config.set<bool>("feral_gamemode", feral_gamemode);
	config.set<bool>("check_update", check_update);
	config.set<bool>("receive_untested_updates", receive_untested_updates);
	config.set<bool>("save_screenshot", save_screenshot);
	config.set<bool>("vk_warning", did_show_vulkan_warning);
	config.set<bool>("gp_download", did_show_graphic_pack_download);
	config.set<bool>("macos_disclaimer", did_show_macos_disclaimer);
	config.set<bool>("fullscreen", fullscreen);
	config.set<bool>("borderless_fullscreen", borderless_fullscreen);

	auto wpos = config.set("window_position");
	wpos.set<sint32>("x", window_position.x);
	wpos.set<sint32>("y", window_position.y);
	auto wsize = config.set("window_size");
	wsize.set<sint32>("x", window_size.x);
	wsize.set<sint32>("y", window_size.y);
	config.set<bool>("window_maximized", window_maximized);

	config.set<bool>("open_pad", pad_open);
	auto ppos = config.set("pad_position");
	ppos.set<sint32>("x", pad_position.x);
	ppos.set<sint32>("y", pad_position.y);
	auto psize = config.set("pad_size");
	psize.set<sint32>("x", pad_size.x);
	psize.set<sint32>("y", pad_size.y);
	config.set<bool>("pad_maximized", pad_maximized);
	config.set<bool>("show_icon_column", show_icon_column);
	config.set("tile_favorites", tile_favorites);
	config.set("tile_hidden", tile_hidden);
	config.set<bool>("tile_show_hidden", tile_show_hidden);
	config.set<sint32>("tile_last_tab", tile_last_tab);
	config.set<sint32>("tile_button_style", tile_button_style);
	config.set<sint32>("tile_sort", tile_sort);
	config.set<sint32>("tile_size", tile_size);
	config.set<sint32>("tile_accent", tile_accent);
	config.set<bool>("tile_volume_done", tile_volume_done);
	config.set("tile_last_selected", tile_last_selected);
	config.set("tile_last_launched", tile_last_launched);
	config.set<bool>("tile_sound", tile_sound);

	auto gamelist = config.set("GameList");
	gamelist.set("style", game_list_style);
	gamelist.set("order", game_list_column_order);
	gamelist.set("name_width", column_width.name);
	gamelist.set("version_width", column_width.version);
	gamelist.set("dlc_width", column_width.dlc);
	gamelist.set("game_time_width", column_width.game_time);
	gamelist.set("game_started_width", column_width.game_started);
	gamelist.set("region_width", column_width.region);
	gamelist.set("title_id", column_width.title_id);

	auto launch_files_parser = config.set("RecentLaunchFiles");
	for (const auto& entry : recent_launch_files)
	{
		launch_files_parser.set("Entry", entry.c_str());
	}

	auto nfc_files_parser = config.set("RecentNFCFiles");
	for (const auto& entry : recent_nfc_files)
	{
		nfc_files_parser.set("Entry", entry.c_str());
	}

	// hotkeys
	auto xml_hotkeys = config.set("Hotkeys");
	xml_hotkeys.set("modifiers", hotkeys.modifiers);
	xml_hotkeys.set("ExitFullscreen", hotkeys.exitFullscreen);
	xml_hotkeys.set("ToggleFullscreen", hotkeys.toggleFullscreen);
	xml_hotkeys.set("ToggleFullscreenAlt", hotkeys.toggleFullscreenAlt);
	xml_hotkeys.set("OpenGameMenu", hotkeys.openGameMenu);
	xml_hotkeys.set("TakeScreenshot", hotkeys.takeScreenshot);
	xml_hotkeys.set("ToggleFastForward", hotkeys.toggleFastForward);
	xml_hotkeys.set("ExitApplication", hotkeys.exitApplication);
}
