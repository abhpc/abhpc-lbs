#ifndef ABHPC_HISTORY_H
#define ABHPC_HISTORY_H

#include <time.h>

struct history {
    time_t updated;
    double fast, slow;
};

void history_advance(struct history *history, time_t now, double score,
                     unsigned fast_minutes, unsigned slow_minutes);

#endif
