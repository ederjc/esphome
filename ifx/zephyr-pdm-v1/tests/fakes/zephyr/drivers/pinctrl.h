#pragma once
struct pinctrl_dev_config {
	int unused;
};
#define PINCTRL_STATE_DEFAULT 0
static inline int pinctrl_apply_state(const struct pinctrl_dev_config *config, int state)
{
	(void)config;
	(void)state;
	return 0;
}
