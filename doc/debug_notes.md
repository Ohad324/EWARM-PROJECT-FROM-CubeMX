# Debug Notes

## INVPC HardFault — Autonomous Investigation Prompt

Use the following prompt with Claude Code to let it autonomously debug the INVPC crash:

> "Autonomously investigate the INVPC HardFault. Read the relevant files, trace the DMA2D ISR call chain, check the
> SDRAM memory overlap between `s_ycbcrBuf` and `frameBuf`, and propose a fix. Work through it step by step without
> waiting for me."
