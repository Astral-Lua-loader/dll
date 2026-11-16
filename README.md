# dll

DLLs the Lua features need. Layout: `<name>/<version>/<name>.dll`; sources in `src/<name>/`.

Lua loads one with `import("lib.dll").load("menu", "1")`; it is cached as `astral\lua\astral_dlls\menu-1.dll`.
To ship a new build add a new version folder and bump the version in Lua (a loaded DLL can't be overwritten, so
versions are never replaced in place).

## menu

The menu (`src/menu`): ImGui over the game via a D3D11 Present hook, shown while Astral's menu is open. The Lua
declares the controls (`ui/menu.lua`), the DLL draws them and keeps values, keybinds and `astral_dlls\menu.cfg`.

    cmake -S src/menu -B src/menu/build -A x64
    cmake --build src/menu/build --config Release     # build/bin/menu.dll + preview.exe
    cd src/menu && build/bin/preview.exe              # offline render (preview_*.bmp) + click checks
