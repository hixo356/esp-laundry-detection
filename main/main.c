#include <stdio.h>

#include "esp_err.h"
#include "esp_log.h"
#include "notifier.h"

void app_main(void)
{
    esp_err_t err = notifier_send(RECIPIENT_BUTTON_1);

    ESP_LOGI("main", "notifier result: %s", esp_err_to_name(err));
}
