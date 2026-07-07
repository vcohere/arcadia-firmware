#include "protocol.h"

void protocol_build(uint8_t steer, uint8_t thr, uint8_t out[PROTOCOL_PACKET_LEN]) {
    out[0] = PKT_START;
    out[1] = steer;
    out[2] = thr;
    out[3] = PKT_CONST3;
    out[4] = PKT_CONST4;
    out[5] = PKT_CONST5;
    out[6] = (uint8_t)(steer ^ thr ^ PKT_CONST3);
    out[7] = PKT_END;
}
