#ifndef _SERIALIZATION_H_
#define _SERIALIZATION_H_

#include "datatypes.h"

#include <mach/mach.h>

typedef struct _spc_mach_message_t {
        mach_msg_header_t header;
        unsigned char buf[];           // variable sized body
} spc_mach_message_t;

// Serializes the given dictionary into a spc_mach_message_t.
// The returned pointer has to be free()d by the caller.
spc_mach_message_t* spc_serialize(spc_message_t* msg);

spc_message_t* spc_deserialize(spc_mach_message_t* msg);

/* Decode a transport-provided CPX byte span, including a received OOL body.
 * Adopts all rights in ports (also on failure) and clears their names. Payload
 * bytes and the table remain caller-owned. Returns NULL and an errno value for
 * invalid/unsupported data; never aborts the receiving service. */
spc_message_t* spc_deserialize_payload(const mach_msg_header_t* header,
    const void* payload, size_t length, spc_port_t* ports, size_t count, int* error);

#endif
