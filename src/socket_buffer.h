#ifndef __SOCKET_BUFFER_H_
#define __SOCKET_BUFFER_H_

#include "types.h"

struct socket_buffer;

struct socket_buffer *socket_buffer_new(process_t self, int fd, int maxsize);
void socket_buffer_delete(struct socket_buffer *b);

int socket_buffer_trash(struct socket_buffer *b, process_t source);
int socket_buffer_event(struct socket_buffer *b);

struct weenet_message;
int socket_buffer_write(struct socket_buffer *b, struct weenet_message *m);

#endif
