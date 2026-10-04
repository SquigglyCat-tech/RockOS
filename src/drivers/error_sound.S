.section .rodata
.balign 4
.global error_sound_start
.global error_sound_end
error_sound_start:
.incbin "assets/error_48000_stereo_s16le.pcm"
error_sound_end:
.section .note.GNU-stack,"",@progbits
