/* The Clear BSD License
 *
 * Copyright (c) 2025 EdgeImpulse Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted (subject to the limitations in the disclaimer
 * below) provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 *
 *   * Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 *
 *   * Neither the name of the copyright holder nor the names of its
 *   contributors may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE GRANTED BY
 * THIS LICENSE. THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/** @note
 * This file contains the porting layer for the Edge Impulse SDK on a STM device in IAR.
 * To use this file with different hardware, please update the hardware-specific implementations.
 */

#include "../ei_classifier_porting.h"
#if EI_PORTING_IAR == 1

#include <stdarg.h>
#include <stdlib.h>
#include <cstdio>

#include "main.h"
#include "stm32f4xx_hal.h"


__attribute__((weak)) EI_IMPULSE_ERROR ei_run_impulse_check_canceled() {
    return EI_IMPULSE_OK;
}

/**
 * Cancelable sleep, can be triggered with signal from other thread
 */
__attribute__((weak)) EI_IMPULSE_ERROR ei_sleep(int32_t time_ms) {

    HAL_Delay(time_ms);

    return EI_IMPULSE_OK;
}

uint64_t ei_read_timer_ms() {

    return HAL_GetTick();
}

uint64_t ei_read_timer_us() {

    return HAL_GetTick() * 1000;
}

__attribute__((weak)) void ei_printf(const char *format, ...) {

    va_list myargs;
    va_start(myargs, format);
    vprintf(format, myargs);
    va_end(myargs);
}

__attribute__((weak)) void ei_printf_float(float f) {
    ei_printf("%f", f);
}

__attribute__((weak)) void ei_putchar(char data)
{
    putchar(data);
}

/* 2026-05-17 (Ohad project edit, NOT EI Studio export):
 * Hard Rule #1 (Absolute Ban on Dynamic Allocation) — these weak defaults
 * are intentionally fail-fast NULL/no-op stubs instead of malloc()/calloc()/
 * free() wrappers. Our project provides STRONG overrides in
 * CM7/Core/Src/wake_word_test.cpp that slice from a static 128 KB pool in
 * .sram1 with LIFO bump-back free. If the strong override ever fails to
 * link (e.g. WAKE_WORD_TEST compile-time gate removed, EI re-export wipes
 * this patch, linker drops the override for some reason), these stubs make
 * the failure LOUD and IMMEDIATE — run_classifier() returns -1002
 * EIDSP_OUT_OF_MEM on the very first allocation — instead of silently
 * falling back to libc malloc and breaking the no-heap guarantee of the
 * whole firmware. Re-apply on every Edge Impulse re-export. */
__attribute__((weak)) void *ei_malloc(size_t size) {
    (void)size;
    return nullptr;
}

__attribute__((weak)) void *ei_calloc(size_t nitems, size_t size) {
    (void)nitems; (void)size;
    return nullptr;
}

__attribute__((weak)) void ei_free(void *ptr) {
    (void)ptr;
}

#if defined(__cplusplus) && EI_C_LINKAGE == 1
extern "C"
#endif
__attribute__((weak)) void DebugLog(const char* s) {
    ei_printf("%s", s);
}

#endif // EI_PORTING_IAR == 1
