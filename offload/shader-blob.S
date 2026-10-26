.section .rodata.hdrfix_shader,"a",@progbits
.balign 16
.global hdrfix_shader_start
.hidden hdrfix_shader_start
hdrfix_shader_start:
.incbin "build/pq.comp.spv"
.global hdrfix_shader_end
.hidden hdrfix_shader_end
hdrfix_shader_end:
.section .note.GNU-stack,"",@progbits
