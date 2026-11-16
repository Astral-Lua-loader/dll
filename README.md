# dll

DLLs the Lua features need. Layout: `<name>/<version>/<name>.dll`, e.g. `example/1/example.dll`.

Lua loads one with `import("lib.dll").load("example", "1")`; it is cached as
`astral\lua\astral_dlls\example-1.dll`. To ship a new build add a new version folder and bump the version in Lua
(a loaded DLL can't be overwritten, so versions are never replaced in place).
