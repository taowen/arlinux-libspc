/* MIT License, see ../LICENSE. The handshake/descriptor order is from
 * connection.c: spc_create_connection_mach_port and spc_accept_connection. */
#include "accept.h"
#include <string.h>

int spc_accept_message(const void *message,size_t length,spc_connection_t *output)
{
    struct {
        mach_msg_header_t header;
        mach_msg_body_t body;
        mach_msg_port_descriptor_t receive_port,send_port;
    } checkin;
    if (!message || !output || length<sizeof(checkin)) return 0;
    memcpy(&checkin,message,sizeof(checkin));
    if (checkin.header.msgh_size!=sizeof(checkin) ||
        checkin.header.msgh_id!=1999646836 ||
        !(checkin.header.msgh_bits&MACH_MSGH_BITS_COMPLEX) ||
        checkin.body.msgh_descriptor_count!=2 ||
        checkin.receive_port.type!=MACH_MSG_PORT_DESCRIPTOR ||
        checkin.send_port.type!=MACH_MSG_PORT_DESCRIPTOR ||
        checkin.receive_port.disposition!=MACH_MSG_TYPE_MOVE_RECEIVE ||
        checkin.send_port.disposition!=MACH_MSG_TYPE_MOVE_SEND ||
        !checkin.receive_port.name || !checkin.send_port.name) return 0;
    output->receive_port=checkin.receive_port.name;
    output->send_port=checkin.send_port.name;
    return 1;
}
