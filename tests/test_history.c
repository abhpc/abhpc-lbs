#include "../src/history.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

int main(void)
{
    struct history whole = {1, 0, 0}, split = whole;
    history_advance(&whole, 3601, 100, 30, 80);
    for (time_t now = 61; now <= 3601; now += 60)
        history_advance(&split, now, 100, 30, 80);
    assert(fabs(whole.fast - split.fast) < 1e-10);
    assert(fabs(whole.slow - split.slow) < 1e-10);

    struct history fractional = {1, 0, 0};
    history_advance(&fractional, 61, 100, 30, 80);
    assert(fractional.slow > 0 && fractional.slow < 1);
    history_advance(&fractional, 601, 100, 30, 80);
    assert(fractional.slow > 1);
    double unchanged = fractional.slow;
    history_advance(&fractional, 601, 0, 30, 80);
    assert(fractional.slow == unchanged);

    /* Include the five-minute Linux load average in the end-to-end response. */
    struct history response = {1, 0, 0};
    double hour = 0, six_hours = 0;
    for (time_t seconds = 5; seconds <= 21600; seconds += 5) {
        double reported = 100 * (1 - exp(-(double)seconds / 300));
        history_advance(&response, seconds + 1, reported, 30, 80);
        if (seconds == 3600)
            hour = response.slow;
        if (seconds == 21600)
            six_hours = response.slow;
    }
    assert(hour > 28 && hour < 31);
    assert(six_hours > 97.5 && six_hours < 98.5);
    struct history falling = {1, 100, 100};
    history_advance(&falling, 21601, 0, 30, 80);
    assert(falling.slow > 1 && falling.slow < 3);
    printf("History response: 1h=%.2f%%, 6h=%.2f%%; elapsed-time and precision checks passed\n",
           hour, six_hours);
    return 0;
}
