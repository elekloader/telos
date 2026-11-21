| SPDX-License-Identifier: GPL-2.0-or-later
| site-check (Digitone II): a TELOS test mod with one code site of its own. It takes the
| view controller's "set dirty" routine (0x4011d5de: moveq #1,d0;
| movea.l 4(sp),a0; move.b d0,32(a0); rts), counts each call after "SITE" in its data,
| and runs the two instructions it replaced. ColdFire V4; -mcpu=54455.
        .section .run, "ax"
        .globl  sc_dirty
sc_dirty:
        addq.l  #1, sc_count
        moveq   #1, %d0
        movea.l 4(%sp), %a0
        jmp     SET_DIRTY + 6

        .section .data
        .balign 4
        .globl  sc_magic
sc_magic:   .ascii  "SITE"
sc_count:   .long   0
