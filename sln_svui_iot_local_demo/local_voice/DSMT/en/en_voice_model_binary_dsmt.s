/*
 * Copyright 2021, 2025 NXP.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

.section .rodata
.align 4

.global en_model_begin

en_model_begin:
.incbin "./en/oob_demo_en_pack.bin"
en_model_end:
