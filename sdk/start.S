/* The entry stub a .t program is linked with: call main_(), then exit rather than running
   off the end of the image into whatever follows it. */
.section ".text.start"
.globl _start
_start:
    bl      main_
    mov     x8, #1              /* SYS_EXIT */
    mov     x0, #0
    svc     #0
1:  b       1b
