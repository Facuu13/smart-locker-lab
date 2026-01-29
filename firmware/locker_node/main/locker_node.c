#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void app_main(void)
{
    printf("LOCKER_NODE: hola mundo!\n");

    while (1) {
        printf("tick\n");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
