# Lunarstone

An unofficial Roblox client for Linux. You can play local places and join live
games. Game support is still rough.

**Roblox can ban your account if you use this to play online.**

## Build

You need GCC 16+, CMake 3.30+, Ninja, Git, pkg-config, Vulkan 1.3, `glslc`,
Wayland (with `wayland-scanner` and wayland-protocols), libxkbcommon, zstd,
xxHash, libsodium, libcrypto, and ICU.

```sh
cmake --preset release
cmake --build --preset release --parallel 4
```

## Play

Set `LUNAR_CONTENT_DIR` to your Roblox `content` folder for fonts and textures.

```sh
build/release/app/lunarstone --play place.rbxl
```

For online play, save your `.ROBLOSECURITY` cookie in
`~/.config/lunarstone/cookie`. Keep that file private. Set `PLACE_ID` to the
place you want to join.

```sh
chmod 600 ~/.config/lunarstone/cookie
build/release/app/lunarstone --net-play "$PLACE_ID"
```

[MIT license](LICENSE)
