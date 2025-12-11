#ifndef CLH_PROTOCOL
#define CLH_PROTOCOL
#include "clh_defs.h"

#define CHANNEL_OFFSET 56
#define SENDER_ID_OFFSET 32
#define CHANNEL_MASK   0b1111111100000000000000000000000000000000000000000000000000000000
#define SENDER_ID_MASK 0b0000000011111111111111111111111100000000000000000000000000000000

clh_u64 clh_encode_tag(clh_u32 channel, clh_i32 sender_id, clh_u32 sender_tag);
void    clh_decode_tag(clh_u64 tag, clh_u32 *channel, clh_i32 *sender_id, clh_u32 *sender_tag);

#endif
