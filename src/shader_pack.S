// The compiled compute shaders (src/gen/shaders.bin, written by tools/shaders.sh), embedded as
// read-only data; src/shaders.cpp reads its table.
    .section .rdata,"dr"
    .p2align 4
    .globl vu_shader_pack
vu_shader_pack:
    .incbin "src/gen/shaders.bin"
    .globl vu_shader_pack_end
vu_shader_pack_end:
    .byte 0
