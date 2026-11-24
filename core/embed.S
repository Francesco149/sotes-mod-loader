/* core/embed.S — Lua sources embedded into version.dll (NUL-terminated). */
    .section .rdata,"dr"
    .globl _oss_lua_dev
_oss_lua_dev:
    .incbin "lua/dev.lua"
    .byte 0
