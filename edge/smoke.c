/* Link gate: create a picoquic server context with TLS, then free it. */
#include <stdio.h>
#include "picoquic.h"
#include "picoquic_utils.h"
int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: smoke cert.pem key.pem\n"); return 2; }
    uint64_t now = picoquic_current_time();
    picoquic_quic_t *q = picoquic_create(8, argv[1], argv[2], NULL, "mcp", NULL, NULL, NULL, NULL, NULL, now, NULL, NULL, NULL, 0);
    if (q == NULL) { printf("create FAILED\n"); return 1; }
    printf("create ok\n");
    picoquic_free(q);
    return 0;
}
