/*
 * boot_pipeline_one_bp.mac.tpl  -- C-SPY macro template, one BP per session.
 *
 * The orchestrator (boot_pipeline_multi.ps1) substitutes @@CHECKPOINT@@
 * with a function name (e.g. "SystemInit", "MX_LTDC_Init") and writes
 * the result as a per-iteration .mac that cspybat consumes.
 *
 * Why a template instead of one big macro:
 *   CSpyBat 9.4.6.1706 (bundled in EWARM 9.70.2) only fires the FIRST
 *   code BP action per invocation, then the session ends with the CPU
 *   halted. Documented behavior; not a config bug. Workaround: one BP
 *   per cspybat run, orchestrated by an outer script.
 */

ReadCycle()
{
    __var c;
    c = __readMemory32(0xE0001004, "Memory");
    __message "    cyc=0x", c:%08X, "\n";
}

DumpKeyRegs()
{
    __var rcc_cr, rcc_cfgr, rcc_pllckselr, rcc_d1cfgr;
    __var rcc_ahb1, rcc_ahb3, rcc_ahb4, rcc_apb2enr, rcc_apb3enr;
    __var flash_acr, pwr_d3cr;
    __var fmc_sdcr1, mpu_ctrl, scb_ccr;
    __var ltdc_gcr, ltdc_isr, ltdc_l1pfcr, ltdc_l1cfbar;
    __var dsi_cr, dsi_wcr;
    __var gpioe_moder, gpioc_moder;

    rcc_cr        = __readMemory32(0x58024400, "Memory");
    rcc_cfgr      = __readMemory32(0x58024410, "Memory");
    rcc_pllckselr = __readMemory32(0x58024428, "Memory");
    rcc_d1cfgr    = __readMemory32(0x58024418, "Memory");
    rcc_ahb1      = __readMemory32(0x580244D8, "Memory");
    rcc_ahb3      = __readMemory32(0x580244D4, "Memory");
    rcc_ahb4      = __readMemory32(0x580244E0, "Memory");
    rcc_apb2enr   = __readMemory32(0x580244F0, "Memory");
    rcc_apb3enr   = __readMemory32(0x580244E4, "Memory");
    flash_acr     = __readMemory32(0x52002000, "Memory");
    pwr_d3cr      = __readMemory32(0x58024818, "Memory");
    fmc_sdcr1     = __readMemory32(0x52004140, "Memory");
    mpu_ctrl      = __readMemory32(0xE000ED94, "Memory");
    scb_ccr       = __readMemory32(0xE000ED14, "Memory");
    ltdc_gcr      = __readMemory32(0x50001018, "Memory");
    ltdc_isr      = __readMemory32(0x50001038, "Memory");
    ltdc_l1pfcr   = __readMemory32(0x50001094, "Memory");
    ltdc_l1cfbar  = __readMemory32(0x500010AC, "Memory");
    dsi_cr        = __readMemory32(0x50000004, "Memory");
    dsi_wcr       = __readMemory32(0x50000024, "Memory");
    gpioe_moder   = __readMemory32(0x58021000, "Memory");
    gpioc_moder   = __readMemory32(0x58020800, "Memory");

    __message "    RCC.CR=0x",   rcc_cr:%08X,        "  CFGR=0x",      rcc_cfgr:%08X,
              "  PLLCKSEL=0x",   rcc_pllckselr:%08X, "  D1CFGR=0x",    rcc_d1cfgr:%08X, "\n";
    __message "    AHB1=0x",     rcc_ahb1:%08X,      "  AHB3=0x",      rcc_ahb3:%08X,
              "  AHB4=0x",       rcc_ahb4:%08X,      "  APB2=0x",      rcc_apb2enr:%08X,
              "  APB3=0x",       rcc_apb3enr:%08X, "\n";
    __message "    FLASH=0x",    flash_acr:%08X,     "  PWR.D3CR=0x",  pwr_d3cr:%08X,
              "  FMC.SDCR1=0x",  fmc_sdcr1:%08X, "\n";
    __message "    MPU.CTRL=0x", mpu_ctrl:%08X,      "  SCB.CCR=0x",   scb_ccr:%08X, "\n";
    __message "    LTDC.GCR=0x", ltdc_gcr:%08X,      "  ISR=0x",       ltdc_isr:%08X,
              "  L1PFCR=0x",     ltdc_l1pfcr:%08X,   "  L1CFBAR=0x",   ltdc_l1cfbar:%08X, "\n";
    __message "    DSI.CR=0x",   dsi_cr:%08X,        "  WCR=0x",       dsi_wcr:%08X, "\n";
    __message "    GPIOE.MODER=0x", gpioe_moder:%08X, "  GPIOC.MODER=0x", gpioc_moder:%08X, "\n";
}

HitCheckpoint()
{
    __message "\n[BP-HIT] @@CHECKPOINT@@ entry\n";
    ReadCycle();
    DumpKeyRegs();
    __message "[BP-HIT] action complete\n";
}

execUserSetup()
{
    __var bp;
    __message "[CSPY] checkpoint='@@CHECKPOINT@@'  installing single BP...\n";
    bp = __setCodeBreak("@@CHECKPOINT@@", 0, "1", "TRUE", "HitCheckpoint()");
    if (bp == 0)
        __message "[CSPY] BP install FAILED -- symbol may be inlined or absent\n";
    else
        __message "[CSPY] BP installed h=", bp:%d, "\n";
}

execUserExit()
{
    __message "[CSPY] session ending\n";
}
