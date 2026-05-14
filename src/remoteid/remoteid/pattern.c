#include "pattern.h"

static const remoteid_pattern_step_t s_solid[] = {
    { true, 1000 },
};

static const remoteid_pattern_step_t s_beacon_1hz_short[] = {
    { true, 100 },
    { false, 900 },
};

static const remoteid_pattern_step_t s_beacon_1hz_50[] = {
    { true, 500 },
    { false, 500 },
};

static const remoteid_pattern_step_t s_strobe_single[] = {
    { true, 60 },
    { false, 1940 },
};

static const remoteid_pattern_step_t s_strobe_double[] = {
    { true, 60 },
    { false, 80 },
    { true, 60 },
    { false, 1800 },
};

static const remoteid_pattern_step_t s_strobe_triple[] = {
    { true, 60 },
    { false, 80 },
    { true, 60 },
    { false, 80 },
    { true, 60 },
    { false, 1660 },
};

static const remoteid_pattern_step_t s_fast_strobe[] = {
    { true, 60 },
    { false, 440 },
};

const remoteid_pattern_step_t *remoteid_pattern_steps(remoteid_pattern_t pattern, size_t *count)
{
    switch (pattern) {
    case REMOTEID_PATTERN_SOLID:
        *count = sizeof(s_solid) / sizeof(s_solid[0]);
        return s_solid;
    case REMOTEID_PATTERN_BEACON_1HZ_50:
        *count = sizeof(s_beacon_1hz_50) / sizeof(s_beacon_1hz_50[0]);
        return s_beacon_1hz_50;
    case REMOTEID_PATTERN_STROBE_SINGLE:
        *count = sizeof(s_strobe_single) / sizeof(s_strobe_single[0]);
        return s_strobe_single;
    case REMOTEID_PATTERN_STROBE_DOUBLE:
        *count = sizeof(s_strobe_double) / sizeof(s_strobe_double[0]);
        return s_strobe_double;
    case REMOTEID_PATTERN_STROBE_TRIPLE:
        *count = sizeof(s_strobe_triple) / sizeof(s_strobe_triple[0]);
        return s_strobe_triple;
    case REMOTEID_PATTERN_FAST_STROBE:
        *count = sizeof(s_fast_strobe) / sizeof(s_fast_strobe[0]);
        return s_fast_strobe;
    case REMOTEID_PATTERN_BEACON_1HZ_SHORT:
    default:
        *count = sizeof(s_beacon_1hz_short) / sizeof(s_beacon_1hz_short[0]);
        return s_beacon_1hz_short;
    }
}

uint32_t remoteid_pattern_period_ms(remoteid_pattern_t pattern)
{
    size_t count = 0;
    const remoteid_pattern_step_t *steps = remoteid_pattern_steps(pattern, &count);
    uint32_t period_ms = 0;

    for (size_t i = 0; i < count; i++) {
        period_ms += steps[i].duration_ms;
    }

    return period_ms;
}

bool remoteid_pattern_is_on(remoteid_pattern_t pattern, uint32_t elapsed_ms, uint32_t phase_ms)
{
    size_t count = 0;
    const remoteid_pattern_step_t *steps = remoteid_pattern_steps(pattern, &count);
    uint32_t period_ms = 0;

    for (size_t i = 0; i < count; i++) {
        period_ms += steps[i].duration_ms;
    }

    if (period_ms == 0) {
        return false;
    }

    uint32_t position_ms = (elapsed_ms % period_ms + phase_ms % period_ms) % period_ms;
    for (size_t i = 0; i < count; i++) {
        if (position_ms < steps[i].duration_ms) {
            return steps[i].on;
        }
        position_ms -= steps[i].duration_ms;
    }

    return false;
}
