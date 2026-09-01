#ifndef USER_SPACE_H
#define USER_SPACE_H

#include "firmware/nvs_manager.h"

void user_space_nvs_update_hook(void);
void user_space_main(void *pvParameters);

#endif // USER_SPACE_H