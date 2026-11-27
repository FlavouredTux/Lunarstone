#pragma once

#include <filesystem>
#include <optional>

// Meshes a place's terrain and writes it as a Wavefront OBJ (with vertex
// colours), optionally with a CPU-rendered PNG preview: top down on the left,
// a tilted view on the right.
int exportTerrain(const std::filesystem::path& placePath, const std::filesystem::path& objPath,
                  const std::optional<std::filesystem::path>& previewPath);
