#ifndef ABHPC_FORMULA_H
#define ABHPC_FORMULA_H

#define FORMULA_MAX 1024

/* Passing NULL values checks syntax without evaluating node-specific arithmetic. */
const char *formula_eval(const char *formula, const double values[4], double *result);

#endif
