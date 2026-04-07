# UM2411 — STM32H747I-DISCO Discovery Kit User Manual
## Section 7.15: Audio — Table 15: SAI4 Pin Assignment

Source: ST UM2411 (Discovery Kits with STM32H747xI and STM32H745xI MCUs), ~page 38.

These are the four mandatory pins for the onboard MEMS microphone (PDM interface) on the STM32H747I-DISCO board.

| Signal Name | Pin Name | Alternate Function | Description |
|-------------|----------|--------------------|-------------|
| SAI4_CK1    | PE2      | AF10               | PDM Clock — physical clock output to the microphone |
| SAI4_D1     | PC1      | AF10               | PDM Data — physical data input from the microphone |
| SAI4_FS_A   | PE4      | AF8                | Frame Sync — internal timing reference (required even in PDM mode) |
| SAI4_SCK_A  | PE5      | AF8                | Serial Clock — internal bit-shifting reference |

## Notes

- **All 4 pins are required** for SAI4 PDM mode. Removing PE4/PE5 is incorrect even though they appear to be "I2S-only" pins — the SAI4 block uses them as internal timing references.
- PE4 and PE5 use **AF8** (SAI4 block A standard pins).
- PE2 and PC1 use **AF10** (SAI4 PDM-specific pins: CK1 = PDM clock generator, D1 = PDM data).
- PE4/PE5 are **not shared** with the LCD or touch controller on this board. They are dedicated to SAI4.
- The BSP driver (`stm32h747i_discovery_audio.c`, `SAI_MspInit()`) configures all 4 pins, which is the authoritative reference for this board.

## Cross-reference: BSP macro definitions

From `stm32h747i_discovery_audio.h`:

```c
#define AUDIO_IN_SAI_PDMx_CLK_IN_PIN         GPIO_PIN_2   // PE2, AF10
#define AUDIO_IN_SAI_PDMx_CLK_IN_PORT        GPIOE
#define AUDIO_IN_SAI_PDMx_DATA_IN_PIN        GPIO_PIN_1   // PC1, AF10
#define AUDIO_IN_SAI_PDMx_DATA_IN_PORT       GPIOC
#define AUDIO_IN_SAI_PDMx_FS_PIN             GPIO_PIN_4   // PE4, AF8
#define AUDIO_IN_SAI_PDMx_SCK_PIN            GPIO_PIN_5   // PE5, AF8
#define AUDIO_IN_SAI_PDMx_FS_SCK_AF          GPIO_AF8_SAI4
#define AUDIO_IN_SAI_PDMx_FS_SCK_GPIO_PORT   GPIOE
```
