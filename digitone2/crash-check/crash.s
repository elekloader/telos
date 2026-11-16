| SPDX-License-Identifier: GPL-2.0-or-later
| crash-check (Digitone II): a TELOS test mod that crashes the unit on its 30th tick
| (about a second in): an illegal instruction in the UI task, so TELOS's crash guard
| restarts into a boot that skips the store's mods. ColdFire V4; -mcpu=54455.
        .section .run, "ax"
        .globl  cc_tick
cc_tick:
        addq.l  #1, cc_n
        moveq   #30, %d0
        cmp.l   cc_n, %d0
        bne.s   1f
        illegal
1:      rts
        .balign 4
cc_n:   .long   0
