#include "protocol.h"

clh_u64 clh_encode_tag(clh_u32 channel, clh_i32 sender_id, clh_u32 sender_tag)
{
    clh_u64 tag = 0;
    tag |= (clh_u64)channel << CHANNEL_OFFSET;
    tag |= (clh_u64)sender_id << SENDER_ID_OFFSET;
    tag |= (clh_u64)sender_tag;
    return tag;
}

void clh_decode_tag(clh_u64 tag, clh_u32 *channel, clh_i32 *sender_id, clh_u32 *sender_tag)
{
    *sender_tag = (clh_u32)tag;
    *channel = (clh_u32)((tag & CHANNEL_MASK) >> CHANNEL_OFFSET);
    *sender_id = (clh_i32)((tag & SENDER_ID_MASK) >> SENDER_ID_OFFSET);
}
