/*
 * Copyright 2024, 2025 NXP
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

.section .rodata
.align 4

.global th_model_begin

th_model_begin:
.incbin "./th/oob_demo_th_pack.bin"
th_model_end:
