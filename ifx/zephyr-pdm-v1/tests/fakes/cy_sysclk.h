#pragma once
static unsigned int fake_hf1 = 8000000;
static inline bool Cy_SysClk_ClkHfIsEnabled(unsigned int index) { return index == 1; }
static inline unsigned int Cy_SysClk_ClkHfGetFrequency(unsigned int index) { return fake_hf1; }
