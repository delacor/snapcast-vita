#ifndef SNAPVITA_CONFIG_H
#define SNAPVITA_CONFIG_H

#include "types.h"

#define CONFIG_DIR  "ux0:data/snapcast"
#define CONFIG_FILE "ux0:data/snapcast/config.json"

void config_default(AppConfig *cfg);
int  config_load(AppConfig *cfg);
int  config_save(const AppConfig *cfg);

#endif /* SNAPVITA_CONFIG_H */
