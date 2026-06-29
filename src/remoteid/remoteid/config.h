#pragma once

#include "sdkconfig.h"

#define REMOTEID_SERVICE_UUID 0xFFFA
#define REMOTEID_AD_APPLICATION_CODE 0x0D

// In runtime-identity mode the static ID strings are not configured; fall back to
// empty so model.c (which only uses them under CONFIG_REMOTEID_IDENTITY_STATIC) and
// any other includer still compiles.
#ifdef CONFIG_REMOTEID_UAS_ID
#define REMOTEID_UAS_ID CONFIG_REMOTEID_UAS_ID
#else
#define REMOTEID_UAS_ID ""
#endif
#ifdef CONFIG_REMOTEID_OPERATOR_ID
#define REMOTEID_OPERATOR_ID CONFIG_REMOTEID_OPERATOR_ID
#else
#define REMOTEID_OPERATOR_ID ""
#endif

#if CONFIG_REMOTEID_HAS_POSITION
#define REMOTEID_TAKEOFF_LATITUDE ((double)CONFIG_REMOTEID_TAKEOFF_LAT_1E6 / 1000000.0)
#define REMOTEID_TAKEOFF_LONGITUDE ((double)CONFIG_REMOTEID_TAKEOFF_LON_1E6 / 1000000.0)
#define REMOTEID_TAKEOFF_ALTITUDE_M ((float)CONFIG_REMOTEID_TAKEOFF_ALT_M)
#endif
