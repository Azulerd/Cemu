#include "wxgui/components/wxGamepadState.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>

#include "input/InputManager.h"
#include "input/api/Controller.h"
#include "input/emulated/VPADController.h"
#include "input/emulated/ProController.h"
#include "input/emulated/ClassicController.h"
#include "wxgui/components/wxGamepadConfig.h"

namespace
{
	constexpr double kStickThreshold = 0.6;

	// L, R and Y only exist on some emulated controller types (the Wiimote alone has none of them)
	template<typename T>
	bool ReadExtraButtons(const EmulatedControllerPtr& controller, wxGamepadState& state)
	{
		const auto typed = std::dynamic_pointer_cast<T>(controller);
		if (!typed)
			return false;
		state.tabPrev |= typed->is_mapping_down(T::kButtonId_L);
		state.tabNext |= typed->is_mapping_down(T::kButtonId_R);
		state.favorite |= typed->is_mapping_down(T::kButtonId_Y);
		return true;
	}
}

int wxDetectGamepadStyle()
{
	auto& inputManager = InputManager::instance();
	for (size_t i = 0; i < InputManager::kMaxController; ++i)
	{
		const EmulatedControllerPtr controller = inputManager.get_controller(i);
		if (!controller)
			continue;
		for (const auto& device : controller->get_controllers())
		{
			if (!device)
				continue;
			const auto api = device->api();
			if (api == InputAPI::Keyboard)
				continue;
			if (api == InputAPI::XInput)
				return 1;
			if (api == InputAPI::Wiimote)
				return 2;
			std::string name = device->display_name();
			std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return (char)std::tolower(c); });
			auto has = [&](const char* text) { return name.find(text) != std::string::npos; };
			if (has("dualsense") || has("dualshock") || has("playstation") || has("ps4") || has("ps5") || has("wireless controller") || has("sony"))
				return 0;
			if (has("xbox") || has("x-box") || has("xinput"))
				return 1;
			if (has("pro controller") || has("nintendo") || has("switch") || has("joy-con") || has("joycon") || has("wii"))
				return 2;
		}
	}
	return 0;
}

wxGamepadState wxReadGamepadState()
{
	wxGamepadState state;
	auto& inputManager = InputManager::instance();
	for (size_t i = 0; i < InputManager::kMaxController; ++i)
	{
		const EmulatedControllerPtr controller = inputManager.get_controller(i);
		if (!controller)
			continue;
		// same reason as in wxGameMenuOverlay: nobody refreshes the state while no game is reading the pad
		controller->controllers_update_states();
		state.start |= controller->is_start_down();
		const glm::vec2 axis = controller->get_axis(); // left stick
		state.up |= controller->is_up_down() || axis.y > kStickThreshold;
		state.down |= controller->is_down_down() || axis.y < -kStickThreshold;
		state.left |= controller->is_left_down() || axis.x < -kStickThreshold;
		state.right |= controller->is_right_down() || axis.x > kStickThreshold;
		const bool a = controller->is_a_down();
		const bool b = controller->is_b_down();
		state.accept |= kAcceptIsWiiUB ? b : a;
		state.back |= kAcceptIsWiiUB ? a : b;

		if (!ReadExtraButtons<ProController>(controller, state))
			if (!ReadExtraButtons<ClassicController>(controller, state))
				ReadExtraButtons<VPADController>(controller, state);
	}
	return state;
}
