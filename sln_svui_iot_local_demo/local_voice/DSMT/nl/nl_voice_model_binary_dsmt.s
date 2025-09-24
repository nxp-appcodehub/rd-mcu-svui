/*
 * Copyright 2024, 2025 NXP
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

.section .rodata
.align 4

.global nl_model_begin

nl_model_begin:
.incbin "./nl/oob_demo_nl_pack.bin"
nl_model_end:
