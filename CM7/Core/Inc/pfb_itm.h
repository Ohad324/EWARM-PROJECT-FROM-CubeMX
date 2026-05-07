/**
 * @file    pfb_itm.h
 * @brief   Bare-metal ITM port 0 character emit -- no CMSIS dependency.
 *
 * Each ITM_PFB(c) call is one MMIO write to the ITM stimulus port 0.
 * No queue, no mutex, no FreeRTOS API, no CMSIS includes. ~30 ns/call.
 * Output captured by JLinkSWOViewer or IAR SWO Trace window.
 *
 * Init expects DEMCR.TRCENA, ITM.LAR unlock, ITM.TCR.ITMENA, ITM.TER.bit0
 * already set -- IAR debugger does this automatically when SWO Trace is
 * configured in project options. If running standalone, call ITM_PFB_Init()
 * once at boot.
 */

#ifndef ITM_PFB_H
#define ITM_PFB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Direct MMIO addresses -- no header chain, no FPU define mess. */
#define ITM_PFB_PORT0_U32   (*(volatile uint32_t *)0xE0000000)
#define ITM_PFB_PORT0_U8    (*(volatile uint8_t  *)0xE0000000)
#define ITM_PFB_TER         (*(volatile uint32_t *)0xE0000E00)
#define ITM_PFB_TCR         (*(volatile uint32_t *)0xE0000E80)
#define ITM_PFB_LAR         (*(volatile uint32_t *)0xE0000FB0)
#define PFB_DEMCR           (*(volatile uint32_t *)0xE000EDFC)

#ifdef RELEASE_BUILD

#define ITM_PFB(c)              ((void)0)
#define ITM_EVENT8(port, value) ((void)0)
#define ITM_EVENT32(port, value) ((void)0)
#define ITM_PFB_Init()          ((void)0)

#else

/* ITM_EVENT8(port, value): emit one byte on chosen stimulus port.
 * Hardware auto-attaches DWT cycle counter when "Generate timestamps"
 * is enabled in IAR's SWO Trace settings. */
#define ITM_EVENT8(port, value) do {                                            \
    if ((ITM_PFB_TCR & 1u) == 0u) break;                                        \
    if ((ITM_PFB_TER & (1u << (port))) == 0u) break;                            \
    while ((*(volatile uint32_t *)(0xE0000000u + 4u * (port))) == 0u) { }      \
    *(volatile uint8_t  *)(0xE0000000u + 4u * (port)) = (uint8_t)(value);      \
} while (0)

/* ITM_EVENT32(port, value): emit 32-bit word on chosen stimulus port.
 * Same auto-timestamp behavior. Use for cycle counts, addresses, masks. */
#define ITM_EVENT32(port, value) do {                                           \
    if ((ITM_PFB_TCR & 1u) == 0u) break;                                        \
    if ((ITM_PFB_TER & (1u << (port))) == 0u) break;                            \
    while ((*(volatile uint32_t *)(0xE0000000u + 4u * (port))) == 0u) { }      \
    *(volatile uint32_t *)(0xE0000000u + 4u * (port)) = (uint32_t)(value);     \
} while (0)

/* legacy alias for the existing port-0 single-byte trace */

/* Enable ITM stimulus port 0 (called once at boot if IAR not attached) */
static inline void ITM_PFB_Init(void)
{
    PFB_DEMCR  |= (1u << 24);     /* TRCENA */
    ITM_PFB_LAR = 0xC5ACCE55u;    /* unlock */
    ITM_PFB_TCR = 1u;             /* ITMENA */
    ITM_PFB_TER = 1u;             /* enable port 0 */
}

/* Emit one byte to ITM port 0. Spins briefly if FIFO full; non-blocking
 * if probe not attached (TER bit 0 will be 0 → bypass the write). */
static inline void ITM_PFB(char c)
{
    /* Skip if ITM not enabled (no probe attached or release build) */
    if ((ITM_PFB_TCR & 1u) == 0u) return;
    if ((ITM_PFB_TER & 1u) == 0u) return;
    /* Spin until FIFO has room */
    while (ITM_PFB_PORT0_U32 == 0u) { /* wait */ }
    ITM_PFB_PORT0_U8 = (uint8_t)c;
}

#endif /* RELEASE_BUILD */

#ifdef __cplusplus
}
#endif

#endif /* ITM_PFB_H */
