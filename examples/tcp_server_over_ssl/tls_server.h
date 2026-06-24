#ifndef __TLS_SERVER_H__
#define __TLS_SERVER_H__

#include <stdint.h>
#include <stddef.h>

int tls_server_init(void);
int tls_server_reset(void);
int tls_server_handshake(void);
int tls_server_write(const unsigned char *buf, size_t len);
int tls_server_read(unsigned char *buf, size_t len);
void tls_server_close_notify(void);
void tls_server_free(void);

#endif /* __TLS_SERVER_H__ */
