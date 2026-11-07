#include "history.h"

#include <math.h>

void history_advance(struct history *history, time_t now, double score,
                     unsigned fast_minutes, unsigned slow_minutes)
{
    double elapsed = difftime(now, history->updated);
    if (elapsed <= 0)
        return;
    double fast_seconds = 60.0 * fast_minutes, slow_seconds = 60.0 * slow_minutes;
    double a = exp(-elapsed / fast_seconds), b = exp(-elapsed / slow_seconds);
    double carry = fast_seconds / (slow_seconds - fast_seconds) * (b - a);
    double incoming = -expm1(-elapsed / slow_seconds) - carry;
    if (incoming < 0)
        incoming = 0;
    history->slow = history->slow * b + history->fast * carry + score * incoming;
    history->fast = history->fast * a + score * -expm1(-elapsed / fast_seconds);
    history->updated = now;
}
