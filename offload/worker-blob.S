.section .rodata.hdrfix_worker,"a",@progbits
.balign 16
.global hdrfix_worker_start
.hidden hdrfix_worker_start
hdrfix_worker_start:
.incbin "build/hdr-offload-worker"
.global hdrfix_worker_end
.hidden hdrfix_worker_end
hdrfix_worker_end:
.section .note.GNU-stack,"",@progbits
