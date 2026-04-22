/*
 * HDMI Scanline Copy - ARM Thumb-2 Assembly (Cortex-M33)
 *
 * Optimized scanline copy for palette-indexed HDMI output.
 * Copies pixels from 8-bit SNES screen buffer (palette index)
 * to 8-bit HDMI line buffer, substituting reserved HDMI sync indices.
 *
 * void hdmi_copy_scanline_asm(
 *     uint8_t* dst,           // r0: destination buffer (HDMI line)
 *     const uint8_t* src,     // r1: source buffer (SNES screen, 8-bit)
 *     uint32_t count,         // r2: pixel count (256)
 *     const uint8_t* subst    // r3: substitution table for indices 251-254
 * );
 */

    .syntax unified
    .cpu cortex-m33
    .thumb
    .section .time_critical.hdmi_asm,"ax",%progbits

    .global hdmi_copy_scanline_asm
    .type hdmi_copy_scanline_asm, %function
    .align 4

/*
 * Register allocation:
 *   r0  = dst pointer (increments)
 *   r1  = src pointer (increments)
 *   r2  = remaining count (decrements)
 *   r3  = substitution table base
 *   r4  = scratch / pixel data
 *   r5  = unused
 *   r6  = constant 251 (BASE_HDMI_CTRL_INX)
 *   r7  = constant 254 (max reserved index)
 */

hdmi_copy_scanline_asm:
    push    {r4-r7, lr}

    /* Load constants */
    movs    r6, #251            /* BASE_HDMI_CTRL_INX */
    movs    r7, #254            /* BASE_HDMI_CTRL_INX + 3 */

    /* Process 8 pixels at a time (main loop) */
    lsrs    r2, r2, #3          /* count /= 8 */
    beq     .L_remainder        /* if count < 8, skip main loop */

.L_loop8:
    /* Load 8 pixels (8 bytes from source = 8 x 8-bit values) */

    /* Pixel 0 */
    ldrb    r4, [r1, #0]        /* load pixel 0 (8-bit) */
    cmp     r4, r6              /* compare with 251 */
    blo     .L_p0_ok
    cmp     r4, r7
    bhi     .L_p0_ok
    subs    r4, r4, r6          /* index = c - 251 */
    ldrb    r4, [r3, r4]        /* substitute */
.L_p0_ok:
    strb    r4, [r0, #0]

    /* Pixel 1 */
    ldrb    r4, [r1, #1]
    cmp     r4, r6
    blo     .L_p1_ok
    cmp     r4, r7
    bhi     .L_p1_ok
    subs    r4, r4, r6
    ldrb    r4, [r3, r4]
.L_p1_ok:
    strb    r4, [r0, #1]

    /* Pixel 2 */
    ldrb    r4, [r1, #2]
    cmp     r4, r6
    blo     .L_p2_ok
    cmp     r4, r7
    bhi     .L_p2_ok
    subs    r4, r4, r6
    ldrb    r4, [r3, r4]
.L_p2_ok:
    strb    r4, [r0, #2]

    /* Pixel 3 */
    ldrb    r4, [r1, #3]
    cmp     r4, r6
    blo     .L_p3_ok
    cmp     r4, r7
    bhi     .L_p3_ok
    subs    r4, r4, r6
    ldrb    r4, [r3, r4]
.L_p3_ok:
    strb    r4, [r0, #3]

    /* Pixel 4 */
    ldrb    r4, [r1, #4]
    cmp     r4, r6
    blo     .L_p4_ok
    cmp     r4, r7
    bhi     .L_p4_ok
    subs    r4, r4, r6
    ldrb    r4, [r3, r4]
.L_p4_ok:
    strb    r4, [r0, #4]

    /* Pixel 5 */
    ldrb    r4, [r1, #5]
    cmp     r4, r6
    blo     .L_p5_ok
    cmp     r4, r7
    bhi     .L_p5_ok
    subs    r4, r4, r6
    ldrb    r4, [r3, r4]
.L_p5_ok:
    strb    r4, [r0, #5]

    /* Pixel 6 */
    ldrb    r4, [r1, #6]
    cmp     r4, r6
    blo     .L_p6_ok
    cmp     r4, r7
    bhi     .L_p6_ok
    subs    r4, r4, r6
    ldrb    r4, [r3, r4]
.L_p6_ok:
    strb    r4, [r0, #6]

    /* Pixel 7 */
    ldrb    r4, [r1, #7]
    cmp     r4, r6
    blo     .L_p7_ok
    cmp     r4, r7
    bhi     .L_p7_ok
    subs    r4, r4, r6
    ldrb    r4, [r3, r4]
.L_p7_ok:
    strb    r4, [r0, #7]

    /* Advance pointers */
    adds    r0, r0, #8          /* dst += 8 */
    adds    r1, r1, #8          /* src += 8 (8-bit values) */

    subs    r2, r2, #1
    bne     .L_loop8

.L_remainder:
    /* No remainder since 256 is divisible by 8 */

    pop     {r4-r7, pc}

    .size hdmi_copy_scanline_asm, . - hdmi_copy_scanline_asm


/*
 * Fast memset for HDMI sync regions
 *
 * void hdmi_memset_fast(uint8_t* dst, uint8_t val, uint32_t count);
 */
    .global hdmi_memset_fast
    .type hdmi_memset_fast, %function
    .align 4

hdmi_memset_fast:
    /* r0 = dst, r1 = val, r2 = count */
    push    {r4-r5, lr}

    /* Replicate byte to word */
    uxtb    r1, r1
    orr     r1, r1, r1, lsl #8   /* val | val<<8 */
    orr     r1, r1, r1, lsl #16  /* val | val<<8 | val<<16 | val<<24 */

    /* Store 4 bytes at a time */
    lsrs    r4, r2, #2           /* count / 4 */
    beq     .L_set_bytes

.L_set_words:
    str     r1, [r0], #4
    subs    r4, r4, #1
    bne     .L_set_words

.L_set_bytes:
    ands    r2, r2, #3           /* remaining bytes */
    beq     .L_set_done

.L_set_tail:
    strb    r1, [r0], #1
    subs    r2, r2, #1
    bne     .L_set_tail

.L_set_done:
    pop     {r4-r5, pc}

    .size hdmi_memset_fast, . - hdmi_memset_fast


/*
 * TMDS serialization for differential HDMI output
 *
 * Converts 3 TMDS-encoded 10-bit values (R, G, B) into a 64-bit
 * serialized differential pair format for PIO output.
 *
 * uint64_t get_ser_diff_data_asm(uint16_t dataR, uint16_t dataG, uint16_t dataB);
 *
 * Input: r0 = dataR (10-bit TMDS), r1 = dataG, r2 = dataB
 * Output: r0 = low 32 bits, r1 = high 32 bits of 64-bit result
 *
 * Algorithm: For each of 10 bit positions (9 down to 0):
 *   - Extract bit from R, G, B
 *   - Convert to differential (bit, ~bit)
 *   - Pack as 6-bit value: (R_diff << 4) | (G_diff << 2) | B_diff
 *   - Shift into 64-bit accumulator
 *   - At position 5, insert 2 extra bits for clock
 */
    .global get_ser_diff_data_asm
    .type get_ser_diff_data_asm, %function
    .align 4

get_ser_diff_data_asm:
    push    {r4-r11, lr}

    /* r0 = dataR, r1 = dataG, r2 = dataB */
    /* Use r3, r4 as 64-bit accumulator (r4 = high, r3 = low) */
    movs    r3, #0              /* out64 low */
    movs    r4, #0              /* out64 high */

    /* r5 = bit position (9 down to 0) */
    movs    r5, #9

    /* r6 = HDMI_PIN_invert_diffpairs (assume 0 for now, can be parameterized) */
    movs    r6, #0

    /* r7 = HDMI_PIN_RGB_notBGR (assume 1 = RGB order) */
    movs    r7, #1

.L_ser_loop:
    /* Shift accumulator left by 6 bits */
    /* out64 <<= 6 means: high = (high << 6) | (low >> 26), low = low << 6 */
    lsls    r4, r4, #6
    lsrs    r8, r3, #26
    orrs    r4, r4, r8
    lsls    r3, r3, #6

    /* At position 5, insert 2 extra bits for clock lane */
    cmp     r5, #4
    bne     .L_no_clock_gap
    /* Shift 2 more bits */
    lsls    r4, r4, #2
    lsrs    r8, r3, #30
    orrs    r4, r4, r8
    lsls    r3, r3, #2
.L_no_clock_gap:

    /* Extract bit from R: bR = (dataR >> bit_pos) & 1 */
    mov     r8, r0
    lsrs    r8, r8, r5
    ands    r8, r8, #1

    /* Extract bit from G: bG = (dataG >> bit_pos) & 1 */
    mov     r9, r1
    lsrs    r9, r9, r5
    ands    r9, r9, #1

    /* Extract bit from B: bB = (dataB >> bit_pos) & 1 */
    mov     r10, r2
    lsrs    r10, r10, r5
    ands    r10, r10, #1

    /* Convert to differential pairs: bit -> (bit, ~bit) = bit | ((bit^1) << 1) */
    /* bR = bR | ((bR ^ 1) << 1) */
    eors    r11, r8, #1
    lsls    r11, r11, #1
    orrs    r8, r8, r11

    /* bG = bG | ((bG ^ 1) << 1) */
    eors    r11, r9, #1
    lsls    r11, r11, #1
    orrs    r9, r9, r11

    /* bB = bB | ((bB ^ 1) << 1) */
    eors    r11, r10, #1
    lsls    r11, r11, #1
    orrs    r10, r10, r11

    /* Apply inversion if HDMI_PIN_invert_diffpairs (skip if r6 == 0) */
    cmp     r6, #0
    beq     .L_no_invert
    eors    r8, r8, #3
    eors    r9, r9, #3
    eors    r10, r10, #3
.L_no_invert:

    /* Pack as d6 = (bR << 4) | (bG << 2) | bB for RGB order */
    /* or d6 = (bB << 4) | (bG << 2) | bR for BGR order */
    cmp     r7, #0
    beq     .L_bgr_order
    /* RGB order */
    lsls    r11, r8, #4
    orrs    r11, r11, r9, lsl #2
    orrs    r11, r11, r10
    b       .L_pack_done
.L_bgr_order:
    /* BGR order */
    lsls    r11, r10, #4
    orrs    r11, r11, r9, lsl #2
    orrs    r11, r11, r8
.L_pack_done:

    /* OR into accumulator low bits */
    orrs    r3, r3, r11

    /* Decrement bit position and loop */
    subs    r5, r5, #1
    bpl     .L_ser_loop

    /* Return 64-bit value: r0 = low, r1 = high */
    mov     r0, r3
    mov     r1, r4

    pop     {r4-r11, pc}

    .size get_ser_diff_data_asm, . - get_ser_diff_data_asm

    .end
