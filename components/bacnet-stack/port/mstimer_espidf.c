#include "bacnet/basic/sys/mstimer.h"
#include "esp_timer.h"

unsigned long mstimer_now(void)
{
    return (unsigned long)(esp_timer_get_time() / 1000ULL);
}
