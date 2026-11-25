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
    .globl _oss_lua_sprites
_oss_lua_sprites:
    .incbin "lua/sprites.lua"
    .byte 0
    .globl _oss_lua_fs
_oss_lua_fs:
    .incbin "lua/fs.lua"
    .byte 0
    .globl _oss_lua_image
_oss_lua_image:
    .incbin "lua/image.lua"
    .byte 0
    .globl _oss_lua_maps
_oss_lua_maps:
    .incbin "lua/maps.lua"
    .byte 0
