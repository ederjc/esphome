#pragma once
#include <stdbool.h>
#include <stdint.h>
#define CY_IP_MXAUDIOSS_VERSION 1
#define CY_PDM_PCM_INTR_RX_TRIGGER 1
#define CY_PDM_PCM_INTR_RX_OVERFLOW 2
#define CY_PDM_PCM_INTR_RX_UNDERFLOW 4
#define CY_PDM_PCM_INTR_MASK 7
#define CY_PDM_PCM_CLK_DIV_BYPASS 0
#define CY_PDM_PCM_OUT_STEREO 3
#define CY_PDM_PCM_SOFT_MUTE_CYCLES_96 1
#define CY_PDM_PCM_WLEN_16_BIT 0
#define CY_PDM_PCM_BYPASS 8
#define CY_PDM_PCM_SUCCESS 0

typedef struct {
	unsigned int clkDiv, mclkDiv, ckoDiv, ckoDelay, sincDecRate, chanSelect;
	bool chanSwapEnable;
	unsigned int highPassFilterGain;
	bool highPassDisable;
	unsigned int softMuteCycles, softMuteFineGain;
	bool softMuteEnable;
	unsigned int wordLen;
	bool signExtension;
	unsigned int gainLeft, gainRight, rxFifoTriggerLevel;
	bool dmaTriggerEnable;
	unsigned int interruptMask;
} cy_stc_pdm_pcm_config_t;

typedef struct {
	uint16_t words[256];
	unsigned int count, position, status, mask;
	bool enabled;
	cy_stc_pdm_pcm_config_t configured;
} PDM_Type;

static inline void Cy_PDM_PCM_SetInterruptMask(PDM_Type *base, unsigned int mask)
{
	base->mask = mask;
}
static inline void Cy_PDM_PCM_Disable(PDM_Type *base) { base->enabled = false; }
static inline void Cy_PDM_PCM_Enable(PDM_Type *base) { base->enabled = true; }
static inline void Cy_PDM_PCM_ClearInterrupt(PDM_Type *base, unsigned int mask)
{
	base->status &= ~mask;
}
static inline unsigned int Cy_PDM_PCM_GetInterruptStatusMasked(PDM_Type *base)
{
	return base->status & base->mask;
}
static inline uint8_t Cy_PDM_PCM_GetNumInFifo(PDM_Type *base)
{
	return base->count - base->position;
}
static inline uint32_t Cy_PDM_PCM_ReadFifo(PDM_Type *base) { return base->words[base->position++]; }
static inline void Cy_PDM_PCM_ClearFifo(PDM_Type *base) { base->count = base->position = 0; }
static inline int Cy_PDM_PCM_Init(PDM_Type *base, const cy_stc_pdm_pcm_config_t *config)
{
	base->configured = *config;
	return CY_PDM_PCM_SUCCESS;
}
