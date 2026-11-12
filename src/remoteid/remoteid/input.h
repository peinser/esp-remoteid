#pragma once

#include <stdbool.h>
#include <stdint.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waddress-of-packed-member"
#include "common/mavlink.h"
#pragma GCC diagnostic pop

// OpenDroneID input messages to store updates, shared by the MAVLink and
// DroneCAN inputs. dronecan.remoteid.* carries the MAVLink OPEN_DRONE_ID_*
// fields with the same units (ArduPilot copies them one to one), so the
// DroneCAN input decodes into the MAVLink structs. `source` names the input in
// log messages.

// True when id_or_mac is set: the message describes another aircraft picked up
// by a Remote ID receiver, not this one.
bool remoteid_input_describes_other_aircraft(const uint8_t id_or_mac[20]);

void remoteid_input_basic_id(const mavlink_open_drone_id_basic_id_t *msg, const char *source);
void remoteid_input_operator_id(const mavlink_open_drone_id_operator_id_t *msg, const char *source);
void remoteid_input_location(const mavlink_open_drone_id_location_t *msg, const char *source);
void remoteid_input_system(const mavlink_open_drone_id_system_t *msg, const char *source);
void remoteid_input_self_id(const mavlink_open_drone_id_self_id_t *msg, const char *source);
