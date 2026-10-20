/* MIT License, see ../LICENSE. Split from connection.c's check-in receiver. */
#ifndef SPC_ACCEPT_H
#define SPC_ACCEPT_H
#include "datatypes.h"
/* Adopt a kernel-received check-in without receiving a second message.
 * No rights are copied or destroyed on failure; the caller owns the message.
 * On success, output owns its actual receive and send descriptor rights. */
int spc_accept_message(const void *message, size_t length, spc_connection_t *output);
#endif
