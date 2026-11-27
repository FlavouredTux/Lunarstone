#pragma once

#include <span>
#include <string_view>

// --net-play <placeId | --replay prefix> [options]: plays a live game in a
// window. Joins the server as --net-join --scripts does (or plays a
// recording of a session back in real time), then each frame takes in what
// the network brought, runs the game's LocalScripts and animations with the
// frame's real time, and draws the replica the way --play draws a place,
// with the game's GUI on top; the camera follows our character. `args`
// starts with the command. Returns the exit code, -1 for bad arguments.
int runNetPlay(std::span<const std::string_view> args);
