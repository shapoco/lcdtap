#pragma once
#include "lcdtap/lcdtap.hpp"

bool loadConfig(lcdtap::LcdTapConfig *out);
void saveConfig(const lcdtap::LcdTapConfig &cfg);
