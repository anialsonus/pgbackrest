/***********************************************************************************************************************************
Configuration Load
***********************************************************************************************************************************/
#ifndef CONFIG_LOAD_H
#define CONFIG_LOAD_H

#include <sys/types.h>

#include "common/type/string.h"

/***********************************************************************************************************************************
Functions
***********************************************************************************************************************************/
void cfgLoad(unsigned int argListSize, const char *argList[]);
void cfgLoadLogFile(void);
void cfgLoadUpdateOption(void);

#endif
