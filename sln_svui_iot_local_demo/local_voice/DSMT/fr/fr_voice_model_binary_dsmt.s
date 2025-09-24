/*
 * Copyright 2021, 2025 NXP.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

.section .rodata
.align 4

.global fr_model_begin

fr_model_begin:
.incbin "./fr/oob_demo_fr_pack.bin"
fr_model_end:
