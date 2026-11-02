#pragma once

// Setting shared by every place that reads the controller for the menus (wxGameMenuOverlay.cpp and wxGamepadState.cpp),
// so both always agree on which button accepts and which one goes back.
//
// PlayStation convention: X (bottom button) accepts and O (right button) goes back.
// The Cemu mapping speaks Wii U: its bottom button is B and its right button is A, so with true:
// B accepts and A goes back. If on your controller they come out swapped, change this to false (only here).
inline constexpr bool kAcceptIsWiiUB = true;
