#pragma once

// Buttons of every connected controller merged into one state, as seen through the Cemu input mapping
// (already translated to Wii U buttons). Used by the library to navigate with the controller.
struct wxGamepadState
{
	bool up = false;
	bool down = false;
	bool left = false;
	bool right = false;
	bool accept = false;   // see kAcceptIsWiiUB in wxGamepadConfig.h
	bool back = false;
	bool start = false;    // + button
	bool favorite = false; // Y
	bool tabPrev = false;  // L
	bool tabNext = false;  // R

	bool Any() const { return up || down || left || right || accept || back || start || favorite || tabPrev || tabNext; }
};

// Layout of the buttons printed in the menu hints: 0 = PlayStation, 1 = Xbox, 2 = Nintendo.
// Guessed from the connected controller (PlayStation when it cannot be told).
int wxDetectGamepadStyle();

wxGamepadState wxReadGamepadState();
