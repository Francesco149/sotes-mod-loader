/* core/embed.S — Lua sources embedded into version.dll (NUL-terminated). */
    .section .rdata,"dr"
    .globl _oss_lua_dev
_oss_lua_dev:
    .incbin "lua/dev.lua"
    .byte 0
    .globl _oss_lua_input
_oss_lua_input:
    .incbin "lua/input.lua"
    .byte 0
    .globl _oss_lua_title
_oss_lua_title:
    .incbin "lua/title.lua"
    .byte 0
    .globl _oss_lua_script
_oss_lua_script:
    .incbin "lua/script.lua"
    .byte 0
