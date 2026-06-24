/**
 * tls_server.c
 *
 * TLS 1.2 server using mbedTLS 3.6 LTS + ATECC608C + W6300
 *
 * Uses an embedded ECDSA test certificate (mbedTLS PolarSSL Test EC).
 * Replace with your own certificate for production use.
 */

#include <stdio.h>
#include <string.h>
#include "tls_server.h"
#include "wizchip_tls.h"
#include "entropy_atecc608.h"

#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include "mbedtls/debug.h"
#include "mbedtls/error.h"

/* ---- Embedded test server certificate (ECDSA P-256) ---- */
static const char srv_cert_pem[] =
    "-----BEGIN CERTIFICATE-----\r\n"
    "MIICIDCCAaWgAwIBAgIBCTAKBggqhkjOPQQDAjA+MQswCQYDVQQGEwJOTDERMA8G\r\n"
    "A1UECgwIUG9sYXJTU0wxHDAaBgNVBAMME1BvbGFyc3NsIFRlc3QgRUMgQ0EwHhcN\r\n"
    "MjMwNTE3MDcxMDM2WhcNMzMwNTE0MDcxMDM2WjA0MQswCQYDVQQGEwJOTDERMA8G\r\n"
    "A1UECgwIUG9sYXJTU0wxEjAQBgNVBAMMCWxvY2FsaG9zdDBZMBMGByqGSM49AgEG\r\n"
    "CCqGSM49AwEHA0IABDfMVtl2CR5acj7HWS3/IG7ufPkGkXTQrRS192giWWKSTuUA\r\n"
    "2CMR/+ov0jRdXRa9iojCa3cNVc2KKg76Aci07f+jgZ0wgZowCQYDVR0TBAIwADAd\r\n"
    "BgNVHQ4EFgQUUGGlj9QH2deCAQzlZX+MY0anE74wbgYDVR0jBGcwZYAUnW0gJEkB\r\n"
    "PyvLeLUZvH4kydv7NnyhQqRAMD4xCzAJBgNVBAYTAk5MMREwDwYDVQQKDAhQb2xh\r\n"
    "clNTTDEcMBoGA1UEAwwTUG9sYXJzc2wgVGVzdCBFQyBDQYIJAMFD4n5iQ8zoMAoG\r\n"
    "CCqGSM49BAMCA2kAMGYCMQDg6p7PPfr2+n7nGvya3pU4ust3k7Obk4/tZX+uHHRQ\r\n"
    "qaccsyULeFNzkyRvWHFeT5sCMQCzDJX79Ii7hILYza/iXWJe/BjJEE8MteCRGXDN\r\n"
    "06jC+BLgOH1KQV9ArqEh3AhOhEg=\r\n"
    "-----END CERTIFICATE-----\r\n";

static const char srv_key_pem[] =
    "-----BEGIN EC PRIVATE KEY-----\r\n"
    "MHcCAQEEIPEqEyB2AnCoPL/9U/YDHvdqXYbIogTywwyp6/UfDw6noAoGCCqGSM49\r\n"
    "AwEHoUQDQgAEN8xW2XYJHlpyPsdZLf8gbu58+QaRdNCtFLX3aCJZYpJO5QDYIxH/\r\n"
    "6i/SNF1dFr2KiMJrdw1VzYoqDvoByLTt/w==\r\n"
    "-----END EC PRIVATE KEY-----\r\n";

/* ---- mbedTLS contexts ---- */
static mbedtls_ssl_context      s_ssl;
static mbedtls_ssl_config       s_conf;
static mbedtls_x509_crt         s_srvcert;
static mbedtls_pk_context       s_srvkey;
static mbedtls_entropy_context  s_entropy;
static mbedtls_ctr_drbg_context s_ctr_drbg;

static void tls_debug_cb(void *ctx, int level,
                          const char *file, int line, const char *str)
{
    (void)ctx;
    const char *p = strrchr(file, '/');
    if (!p) p = strrchr(file, '\\');
    p = p ? p + 1 : file;
    printf("[mbedTLS:%d] %s:%d: %s", level, p, line, str);
}

int tls_server_init(void)
{
    int ret;

    /* 1. Entropy + DRBG */
    mbedtls_entropy_init(&s_entropy);
    mbedtls_ctr_drbg_init(&s_ctr_drbg);

    ret = mbedtls_entropy_add_source(&s_entropy, entropy_atecc608_source,
                                     NULL, 32, MBEDTLS_ENTROPY_SOURCE_STRONG);
    if (ret != 0) {
        printf("[TLS] entropy_add_source failed: -0x%04X\r\n", (unsigned)-ret);
        return ret;
    }

    ret = mbedtls_ctr_drbg_seed(&s_ctr_drbg, mbedtls_entropy_func,
                                &s_entropy,
                                (const unsigned char *)"tls_server", 10);
    if (ret != 0) {
        printf("[TLS] ctr_drbg_seed failed: -0x%04X\r\n", (unsigned)-ret);
        return ret;
    }
    printf("[TLS] RNG seeded OK\r\n");

    /* 2. Server certificate + private key */
    mbedtls_x509_crt_init(&s_srvcert);
    mbedtls_pk_init(&s_srvkey);

    ret = mbedtls_x509_crt_parse(&s_srvcert,
                                  (const unsigned char *)srv_cert_pem,
                                  sizeof(srv_cert_pem));
    if (ret != 0) {
        printf("[TLS] cert parse failed: -0x%04X\r\n", (unsigned)-ret);
        return ret;
    }
    printf("[TLS] Server cert loaded\r\n");

    ret = mbedtls_pk_parse_key(&s_srvkey,
                                (const unsigned char *)srv_key_pem,
                                sizeof(srv_key_pem),
                                NULL, 0,
                                mbedtls_ctr_drbg_random, &s_ctr_drbg);
    if (ret != 0) {
        printf("[TLS] key parse failed: -0x%04X\r\n", (unsigned)-ret);
        return ret;
    }
    printf("[TLS] Server key loaded\r\n");

    /* 3. SSL config */
    mbedtls_ssl_init(&s_ssl);
    mbedtls_ssl_config_init(&s_conf);

    ret = mbedtls_ssl_config_defaults(&s_conf,
                                       MBEDTLS_SSL_IS_SERVER,
                                       MBEDTLS_SSL_TRANSPORT_STREAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        printf("[TLS] ssl_config_defaults failed: -0x%04X\r\n", (unsigned)-ret);
        return ret;
    }

    mbedtls_ssl_conf_authmode(&s_conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&s_conf, mbedtls_ctr_drbg_random, &s_ctr_drbg);
    mbedtls_ssl_conf_own_cert(&s_conf, &s_srvcert, &s_srvkey);

    mbedtls_ssl_conf_dbg(&s_conf, tls_debug_cb, NULL);
    mbedtls_debug_set_threshold(1);

    ret = mbedtls_ssl_setup(&s_ssl, &s_conf);
    if (ret != 0) {
        printf("[TLS] ssl_setup failed: -0x%04X\r\n", (unsigned)-ret);
        return ret;
    }

    mbedtls_ssl_set_bio(&s_ssl, wizchip_tls_get_ctx(),
                        wizchip_tls_send, wizchip_tls_recv, NULL);

    printf("[TLS] Server init complete\r\n");
    return 0;
}

int tls_server_reset(void)
{
    int ret = mbedtls_ssl_session_reset(&s_ssl);
    if (ret != 0) {
        printf("[TLS] session_reset failed: -0x%04X\r\n", (unsigned)-ret);
        return ret;
    }
    mbedtls_ssl_set_bio(&s_ssl, wizchip_tls_get_ctx(),
                        wizchip_tls_send, wizchip_tls_recv, NULL);
    return 0;
}

int tls_server_handshake(void)
{
    int ret;
    char errbuf[128];

    printf("[TLS] Starting handshake...\r\n");
    while ((ret = mbedtls_ssl_handshake(&s_ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
            ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            mbedtls_strerror(ret, errbuf, sizeof(errbuf));
            printf("[TLS] Handshake failed: -0x%04X (%s)\r\n",
                   (unsigned)-ret, errbuf);
            return ret;
        }
    }

    printf("[TLS] Handshake OK!\r\n");
    printf("[TLS] Ciphersuite: %s\r\n", mbedtls_ssl_get_ciphersuite(&s_ssl));
    printf("[TLS] TLS version: %s\r\n", mbedtls_ssl_get_version(&s_ssl));
    return 0;
}

int tls_server_write(const unsigned char *buf, size_t len)
{
    int ret;
    while ((ret = mbedtls_ssl_write(&s_ssl, buf, len)) <= 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_WRITE)
            return ret;
    }
    return ret;
}

int tls_server_read(unsigned char *buf, size_t len)
{
    int ret;
    while ((ret = mbedtls_ssl_read(&s_ssl, buf, len)) <= 0) {
        if (ret == 0)
            return 0;
        if (ret != MBEDTLS_ERR_SSL_WANT_READ)
            return ret;
    }
    return ret;
}

void tls_server_close_notify(void)
{
    mbedtls_ssl_close_notify(&s_ssl);
}

void tls_server_free(void)
{
    mbedtls_ssl_free(&s_ssl);
    mbedtls_ssl_config_free(&s_conf);
    mbedtls_x509_crt_free(&s_srvcert);
    mbedtls_pk_free(&s_srvkey);
    mbedtls_ctr_drbg_free(&s_ctr_drbg);
    mbedtls_entropy_free(&s_entropy);
    printf("[TLS] Server freed\r\n");
}
