// Host harness for the DroneCAN wire format, driven by dronecan_test.py, which
// checks it against the reference pydronecan implementation in both directions.
//   dronecan_test rx   reads "<can id hex> <data hex>" frames on stdin and prints
//                      what the firmware decoded (Remote ID messages, arm state,
//                      clock seed) and transmitted (GetNodeInfo responses).
//   dronecan_test tx   prints the periodic NodeStatus and ArmStatus frames (ready
//                      and not ready) and a GetNodeInfo response.
#include "../../src/remoteid/remoteid/dronecan.c"

int64_t g_now_us = 5000000;
static remoteid_health_t g_health = REMOTEID_HEALTH_OK;

esp_err_t twai_transmit(const twai_message_t *message, TickType_t ticks)
{
    (void)ticks;
    printf("TX %08x ", (unsigned)message->identifier);
    for (int i = 0; i < message->data_length_code; i++) {
        printf("%02x", message->data[i]);
    }
    printf("\n");
    return ESP_OK;
}

remoteid_health_t remoteid_store_health(void) { return g_health; }
const char *remoteid_store_health_text(remoteid_health_t health)
{
    return health == REMOTEID_HEALTH_OK ? "" : "Operator ID not set";
}
esp_err_t remoteid_store_submit(const remoteid_store_update_t *update, TickType_t timeout)
{
    (void)timeout;
    if (update->type == REMOTEID_STORE_UPDATE_ARMED) {
        printf("ARMED %d\n", update->data.armed.armed);
    }
    return ESP_OK;
}
bool remoteid_clock_is_synced(void) { return false; }
void remoteid_clock_set_utc_us(uint64_t unix_us) { printf("CLOCK %llu\n", (unsigned long long)unix_us); }

static void print_hex(const char *key, const void *data, size_t len)
{
    const uint8_t *bytes = data;
    printf("%s=", key);
    for (size_t i = 0; i < len; i++) {
        printf("%02x", bytes[i]);
    }
    printf(" ");
}

bool remoteid_input_describes_other_aircraft(const uint8_t id_or_mac[20])
{
    for (int i = 0; i < 20; i++) {
        if (id_or_mac[i]) {
            return true;
        }
    }
    return false;
}
void remoteid_input_basic_id(const mavlink_open_drone_id_basic_id_t *m, const char *source)
{
    (void)source;
    printf("BASIC id_type=%u ua_type=%u ", m->id_type, m->ua_type);
    print_hex("uas_id", m->uas_id, 20);
    printf("\n");
}
void remoteid_input_operator_id(const mavlink_open_drone_id_operator_id_t *m, const char *source)
{
    (void)source;
    printf("OPERATOR type=%u ", m->operator_id_type);
    print_hex("id", m->operator_id, 20);
    printf("\n");
}
void remoteid_input_self_id(const mavlink_open_drone_id_self_id_t *m, const char *source)
{
    (void)source;
    printf("SELF type=%u ", m->description_type);
    print_hex("desc", m->description, 23);
    printf("\n");
}
void remoteid_input_location(const mavlink_open_drone_id_location_t *m, const char *source)
{
    (void)source;
    printf("LOCATION status=%u direction=%u speed_horizontal=%u speed_vertical=%d latitude=%d longitude=%d "
           "altitude_barometric=%.3f altitude_geodetic=%.3f height_reference=%u height=%.3f horizontal_accuracy=%u "
           "vertical_accuracy=%u barometer_accuracy=%u speed_accuracy=%u timestamp=%.3f timestamp_accuracy=%u\n",
           m->status, m->direction, m->speed_horizontal, m->speed_vertical, m->latitude, m->longitude,
           m->altitude_barometric, m->altitude_geodetic, m->height_reference, m->height, m->horizontal_accuracy,
           m->vertical_accuracy, m->barometer_accuracy, m->speed_accuracy, m->timestamp, m->timestamp_accuracy);
}
void remoteid_input_system(const mavlink_open_drone_id_system_t *m, const char *source)
{
    (void)source;
    printf("SYSTEM operator_location_type=%u classification_type=%u operator_latitude=%d operator_longitude=%d "
           "area_count=%u area_radius=%u area_ceiling=%.3f area_floor=%.3f category_eu=%u class_eu=%u "
           "operator_altitude_geo=%.3f timestamp=%u\n",
           m->operator_location_type, m->classification_type, m->operator_latitude, m->operator_longitude,
           m->area_count, m->area_radius, m->area_ceiling, m->area_floor, m->category_eu, m->class_eu,
           m->operator_altitude_geo, m->timestamp);
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "tx") == 0) {
        send_status();
        g_health = REMOTEID_HEALTH_WAITING_OPERATOR_ID;
        send_status();
        dronecan_transfer_meta_t request = { .priority = 30, .data_type_id = DTID_GET_NODE_INFO, .source_node_id = 10,
                                             .service = true, .request = true, .destination_node_id = 100,
                                             .start_of_transfer = true, .end_of_transfer = true, .transfer_id = 7 };
        respond_node_info(&request);
        return 0;
    }
    char line[256];
    while (fgets(line, sizeof(line), stdin)) {
        twai_message_t frame = { .extd = 1 };
        char data[64] = "";
        unsigned id;
        if (sscanf(line, "%x %63s", &id, data) < 1) {
            continue;
        }
        frame.identifier = id;
        frame.data_length_code = (uint8_t)(strlen(data) / 2);
        for (int i = 0; i < frame.data_length_code; i++) {
            sscanf(&data[2 * i], "%2hhx", &frame.data[i]);
        }
        handle_frame(&frame);
    }
    return 0;
}
