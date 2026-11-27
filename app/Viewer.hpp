#pragma once

#include <filesystem>

// Opens a window and flies through a place: hold the right mouse button to
// look around, WASD/QE to move, Shift to go faster, F11 for fullscreen.
int viewPlace(const std::filesystem::path& path);
