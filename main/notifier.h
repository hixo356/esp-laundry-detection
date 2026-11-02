#ifndef NOTIFIER_H
#define NOTIFIER_H

#include "esp_err.h"

typedef enum{
    RECIPIENT_BUTTON_1,
    RECIPIENT_BUTTON_2,
} notifier_recipient_t;

esp_err_t notifier_send(notifier_recipient_t recipient);

#endif