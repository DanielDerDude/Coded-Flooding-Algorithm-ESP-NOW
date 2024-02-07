#ifndef SIMPLE_LINEAR_REGRESSION_H
#define SIMPLE_LINEAR_REGRESSION_H

#ifndef _INCLUDES_H_
#define _INCLUDES_H_
#include "../../includes.h"
#endif

enum {
    LINREG_OKAY       = 0,
    ERROR_INPUT_VALUE = 1,
    ERROR_NUMERIC     = 2,
};

int simple_linear_regression(const float * x, const float * y, const int n, float * slope_out, float * intercept_out, float * r2_out, float * mae_out, float * mse_out, float * rmse_out) {
    /* Use double precision for the accumulators - they can grow large */
    double sum_x = 0.0;
    double sum_xx = 0.0;
    double sum_xy = 0.0;
    double sum_y = 0.0;
    double sum_yy = 0.0;
    double n_real = (double)(n);
    int i = 0;
    double slope = 0.0;
    double intercept = 0.0;
    double denominator = 0.0;
    double err = 0.0;
    double ack = 0.0;

    if (x == NULL || y == NULL || n < 2) {
        return ERROR_INPUT_VALUE;
    }

    for (i = 0; i < n; ++i) {
        sum_x += (double)(x[i]);
        sum_xx += (double)(x[i]) * (double)(x[i]);
        sum_xy += (double)(x[i]) * (double)(y[i]);
        sum_y += (double)(y[i]);
        sum_yy += (double)(y[i]) * (double)(y[i]);
    }

    denominator = n_real * sum_xx - sum_x * sum_x;
    if (denominator == 0.0) {
        return ERROR_NUMERIC;
    }
    slope = (n_real * sum_xy - sum_x * sum_y) / denominator;

    if (slope_out != NULL) {
        *slope_out = (float)(slope);
    }

    intercept = (sum_y - slope * sum_x) / n_real;
    if (intercept_out != NULL) {
        *intercept_out = (float)(intercept);
    }

    if (r2_out != NULL) {
        denominator = ((n_real * sum_xx) - (sum_x * sum_x)) * ((n_real * sum_yy) - (sum_y * sum_y));
        if (denominator == 0.0) {
            return ERROR_NUMERIC;
        }
        *r2_out = (float)(((n_real * sum_xy) - (sum_x * sum_y)) * ((n_real * sum_xy) - (sum_x * sum_y)) / denominator);
    }

    if (mae_out != NULL) {
        for (i = 0; i < n; ++i) {
            err = intercept + (double)(x[i]) * slope - (double)(y[i]);
            ack += fabs(err);
        }
        *mae_out = (float)(ack / n_real);
    }

    if (mse_out != NULL || rmse_out != NULL) {
        ack = 0.0;
        for (i = 0; i < n; ++i) {
            err = intercept + (double)(x[i]) * slope - (double)(y[i]);
            ack += err * err;
        }
        if (mse_out != NULL) {
            *mse_out = (float)(ack / n_real);
        }
        if (rmse_out != NULL) {
            *rmse_out = (float)(sqrt(ack / n_real));
        }
    }

    return LINREG_OKAY;
}

#endif