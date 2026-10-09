/* The MCP edge: one QUIC endpoint for xr_pilot_mcp, mutual TLS under one root.
 *
 *   mcp_edge serve --cert C --key K --root R --allow CN[,CN...] --port P --upstream PORT
 *   mcp_edge call  --cert C --key K --root R --server-name NAME --connect HOST:PORT
 *
 * serve requires a client certificate that chains to R and whose subject CN is on
 * the allow list; anything else fails the handshake. Each bidirectional stream
 * carries one JSON-RPC message; a worker thread POSTs it to 127.0.0.1:PORT/mcp and
 * the reply goes back on the same stream. At most EDGE_CREDITS requests run at
 * once; a request beyond that is answered "edge busy" at once, never queued.
 *
 * call is a stdio MCP transport: each stdin line is one message on its own
 * stream, each reply is one stdout line. It accepts only a server whose
 * certificate chains to R and names NAME. ALPN is "xr-mcp". */
#ifdef _WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define closesocket close
#endif
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include "picoquic.h"
#include "picoquic_packet_loop.h"
#include "picoquic_utils.h"
#include "picoquic_set_textlog.h"
#include "picotls.h"
#include "picotls/openssl.h"

#define EDGE_ALPN "xr-mcp"
#define EDGE_CREDITS 4
#define EDGE_MAX_MSG (128u << 20)
#define EDGE_IDLE_MS 120000

typedef struct { uint8_t *p; size_t n, cap; } buf_t;

static void edge_sleep_ms(int ms)
{
#ifdef _WINDOWS
    Sleep((DWORD)ms);
#else
    usleep((useconds_t)ms * 1000);
#endif
}

/* "edge busy", answered at once, carrying the request's id so the caller can match it. */
static void reply_busy(picoquic_cnx_t *cnx, uint64_t sid, const buf_t *req)
{
    char id[128] = "null", out[256];
    const char *p = req->n ? memchr(req->p, '{', req->n) : NULL, *end = (const char *)req->p + req->n;
    for (const char *k = p; k && k + 4 < end; k++)
        if (memcmp(k, "\"id\"", 4) == 0) {
            const char *v = k + 4;
            while (v < end && (*v == ' ' || *v == ':'))
                v++;
            const char *e = v;
            if (e < end && *e == '"')
                for (e++; e < end && *e != '"'; e++)
                    if (*e == '\\')
                        e++;
            while (e < end && *e != ',' && *e != '}')
                e++;
            if (e > v && (size_t)(e - v) < sizeof(id)) {
                memcpy(id, v, (size_t)(e - v));
                id[e - v] = 0;
            }
            break;
        }
    int n = snprintf(out, sizeof(out), "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":-32000,\"message\":\"edge busy\"}}", id);
    picoquic_add_to_stream(cnx, sid, (const uint8_t *)out, (size_t)n, 1);
}

static int buf_add(buf_t *b, const void *d, size_t n)
{
    if (b->n + n > EDGE_MAX_MSG)
        return -1;
    if (b->n + n > b->cap) {
        size_t c = b->cap ? b->cap : 4096;
        while (c < b->n + n)
            c *= 2;
        uint8_t *q = realloc(b->p, c);
        if (q == NULL)
            return -1;
        b->p = q;
        b->cap = c;
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
    return 0;
}

static void logf_(const char *fmt, const char *a, const char *b)
{
    fprintf(stderr, "mcp_edge: ");
    fprintf(stderr, fmt, a ? a : "", b ? b : "");
    fputc('\n', stderr);
    fflush(stderr);
}

/* ---------- the allow list, checked after the chain verifies ---------- */

typedef struct {
    ptls_openssl_override_verify_certificate_t super;
    char **allow;
    int nallow;
} allow_t;

static int allow_cb(ptls_openssl_override_verify_certificate_t *self, ptls_t *tls, int ret, int ossl_ret, X509 *cert,
                    STACK_OF(X509) *chain)
{
    (void)tls;
    (void)chain;
    allow_t *a = (allow_t *)self;
    char cn[256] = "";
    if (cert != NULL)
        X509_NAME_get_text_by_NID(X509_get_subject_name(cert), NID_commonName, cn, sizeof(cn));
    if (cert == NULL) {
        logf_("refused%s%s: no client certificate", NULL, NULL);
        return PTLS_ALERT_CERTIFICATE_REQUIRED;
    }
    if (ret != 0) {
        logf_("refused %s: chain does not verify (%s)", cn, X509_verify_cert_error_string(ossl_ret));
        return ret;
    }
    for (int i = 0; i < a->nallow; i++)
        if (strcmp(cn, a->allow[i]) == 0) {
            logf_("admitted %s%s", cn, NULL);
            return 0;
        }
    logf_("refused %s: not on the allow list%s", cn, NULL);
    return PTLS_ALERT_BAD_CERTIFICATE;
}

static int install_allow_list(picoquic_quic_t *quic, const char *root, char *list)
{
    static ptls_openssl_verify_certificate_t verifier;
    static allow_t allow;
    X509_STORE *store = X509_STORE_new();
    if (store == NULL || X509_STORE_load_locations(store, root, NULL) != 1)
        return -1;
    for (char *t = strtok(list, ","); t != NULL; t = strtok(NULL, ","))
        if ((allow.allow = realloc(allow.allow, sizeof(char *) * (allow.nallow + 1))) != NULL)
            allow.allow[allow.nallow++] = t;
    if (allow.nallow == 0 || ptls_openssl_init_verify_certificate(&verifier, store) != 0)
        return -1;
    allow.super.cb = allow_cb;
    verifier.override_callback = &allow.super;
    picoquic_set_verify_certificate_callback(quic, &verifier.super, NULL);
    picoquic_set_client_authentication(quic, 1);
    return 0;
}

/* ---------- serve: streams in, worker threads to the MCP server, replies out ---------- */

typedef struct conn_s {
    picoquic_cnx_t *cnx;
    int closed, pending;
    struct conn_s *next;
} conn_t;

typedef struct job_s {
    conn_t *conn;
    uint64_t stream_id;
    buf_t req, resp;
    struct job_s *next;
} job_t;

static struct {
    uint16_t upstream;
    picoquic_network_thread_ctx_t *thread;
    pthread_mutex_t mu;
    job_t *done;
    int credits;
} S = {.mu = PTHREAD_MUTEX_INITIALIZER, .credits = EDGE_CREDITS};

/* POST the message to the MCP server; the reply body, decoded from chunked or SSE framing. */
static void forward(job_t *j)
{
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(S.upstream);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    buf_t raw = {0};
    char hdr[256];
    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        const char *e = "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32001,\"message\":\"upstream unreachable\"}}";
        buf_add(&j->resp, e, strlen(e));
        if (fd >= 0)
            closesocket(fd);
        return;
    }
    int hn = snprintf(hdr, sizeof(hdr),
                      "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
                      "Accept: application/json, text/event-stream\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                      j->req.n);
    send(fd, hdr, hn, 0);
    for (size_t off = 0; off < j->req.n;) {
        int w = send(fd, (const char *)j->req.p + off, (int)(j->req.n - off), 0);
        if (w <= 0)
            break;
        off += (size_t)w;
    }
    char tmp[65536];
    for (int r; (r = recv(fd, tmp, sizeof(tmp), 0)) > 0;)
        if (buf_add(&raw, tmp, (size_t)r) != 0)
            break;
    closesocket(fd);
    buf_add(&raw, "", 1);
    char *s = (char *)raw.p, *body = s ? strstr(s, "\r\n\r\n") : NULL;
    if (body == NULL) {
        free(raw.p);
        return;
    }
    *body = 0;
    body += 4;
    for (char *c = s; *c; c++)
        if (*c >= 'A' && *c <= 'Z')
            *c += 32;
    int status = atoi(strchr(s, ' ') ? strchr(s, ' ') + 1 : "0");
    if (status == 202 || status == 204) {
        free(raw.p);
        return;
    }
    if (strstr(s, "transfer-encoding: chunked")) {
        char *o = body, *i = body;
        for (unsigned long n; (n = strtoul(i, &i, 16)) > 0;) {
            i = strstr(i, "\r\n");
            if (i == NULL)
                break;
            i += 2;
            memmove(o, i, n);
            o += n;
            i += n + 2;
        }
        *o = 0;
    }
    if (strstr(s, "content-type: text/event-stream")) {
        for (char *l = body; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL)
            if (strncmp(l, "data:", 5) == 0) {
                char *v = l + 5, *e = strpbrk(v, "\r\n");
                while (*v == ' ')
                    v++;
                buf_add(&j->resp, v, e ? (size_t)(e - v) : strlen(v));
            }
    } else {
        size_t n = strlen(body);
        while (n && (body[n - 1] == '\n' || body[n - 1] == '\r'))
            n--;
        buf_add(&j->resp, body, n);
    }
    free(raw.p);
}

static void *worker(void *arg)
{
    job_t *j = arg;
    forward(j);
    pthread_mutex_lock(&S.mu);
    j->next = S.done;
    S.done = j;
    pthread_mutex_unlock(&S.mu);
    picoquic_wake_up_network_thread(S.thread);
    return NULL;
}

static void job_free(job_t *j)
{
    free(j->req.p);
    free(j->resp.p);
    free(j);
}

static void conn_release(conn_t *c)
{
    if (--c->pending == 0 && c->closed)
        free(c);
}

static int serve_cb(picoquic_cnx_t *cnx, uint64_t sid, uint8_t *bytes, size_t len, picoquic_call_back_event_t ev,
                    void *cctx, void *sctx)
{
    conn_t *c = cctx;
    if (c == NULL || c == picoquic_get_default_callback_context(picoquic_get_quic_ctx(cnx))) {
        if ((c = calloc(1, sizeof(*c))) == NULL)
            return -1;
        c->cnx = cnx;
        c->pending = 1; /* held by the connection itself */
        picoquic_set_callback(cnx, serve_cb, c);
    }
    job_t *j = sctx;
    switch (ev) {
    case picoquic_callback_stream_data:
    case picoquic_callback_stream_fin:
        if (j == NULL) {
            if ((j = calloc(1, sizeof(*j))) == NULL)
                return -1;
            j->conn = c;
            j->stream_id = sid;
            picoquic_set_app_stream_ctx(cnx, sid, j);
        }
        if (len && buf_add(&j->req, bytes, len) != 0) {
            picoquic_reset_stream(cnx, sid, 1);
            return 0;
        }
        if (ev == picoquic_callback_stream_fin) {
            picoquic_set_app_stream_ctx(cnx, sid, NULL);
            pthread_t t;
            if (S.credits == 0) {
                reply_busy(cnx, sid, &j->req);
                job_free(j);
                return 0;
            }
            S.credits--;
            c->pending++;
            if (pthread_create(&t, NULL, worker, j) != 0) {
                S.credits++;
                c->pending--;
                reply_busy(cnx, sid, &j->req);
                job_free(j);
                return 0;
            }
            pthread_detach(t);
        }
        return 0;
    case picoquic_callback_stream_reset:
        if (j != NULL) {
            picoquic_set_app_stream_ctx(cnx, sid, NULL);
            job_free(j);
        }
        return 0;
    case picoquic_callback_close:
    case picoquic_callback_application_close:
    case picoquic_callback_stateless_reset:
        c->closed = 1;
        c->cnx = NULL;
        picoquic_set_callback(cnx, NULL, NULL);
        conn_release(c);
        return 0;
    default:
        return 0;
    }
}

static int serve_loop_cb(picoquic_quic_t *quic, picoquic_packet_loop_cb_enum mode, void *ctx, void *arg)
{
    (void)quic;
    (void)ctx;
    (void)arg;
    if (mode != picoquic_packet_loop_wake_up)
        return 0;
    pthread_mutex_lock(&S.mu);
    job_t *j = S.done;
    S.done = NULL;
    pthread_mutex_unlock(&S.mu);
    while (j != NULL) {
        job_t *n = j->next;
        S.credits++;
        /* A notification has no reply. picoquic schedules a send only when a write has
         * bytes, so a bare FIN would never leave; one newline carries it. */
        if (!j->conn->closed) {
            if (j->resp.n == 0)
                picoquic_add_to_stream(j->conn->cnx, j->stream_id, (const uint8_t *)"\n", 1, 1);
            else
                picoquic_add_to_stream(j->conn->cnx, j->stream_id, j->resp.p, j->resp.n, 1);
        }
        conn_release(j->conn);
        job_free(j);
        j = n;
    }
    return 0;
}

/* ---------- call: stdin lines out, replies to stdout ---------- */

typedef struct line_s {
    buf_t b;
    struct line_s *next;
} line_t;

static struct {
    picoquic_network_thread_ctx_t *thread;
    picoquic_cnx_t *cnx;
    pthread_mutex_t mu;
    line_t *out;
    int inflight, ready, failed, eof;
    volatile int done;
} C = {.mu = PTHREAD_MUTEX_INITIALIZER};

static int call_cb(picoquic_cnx_t *cnx, uint64_t sid, uint8_t *bytes, size_t len, picoquic_call_back_event_t ev,
                   void *cctx, void *sctx)
{
    (void)cctx;
    buf_t *b = sctx;
    switch (ev) {
    case picoquic_callback_ready:
        C.ready = 1;
        return 0;
    case picoquic_callback_stream_data:
    case picoquic_callback_stream_fin:
        if (b == NULL) {
            if ((b = calloc(1, sizeof(*b))) == NULL)
                return -1;
            picoquic_set_app_stream_ctx(cnx, sid, b);
        }
        if (len)
            buf_add(b, bytes, len);
        if (ev == picoquic_callback_stream_fin) {
            while (b->n && (b->p[b->n - 1] == '\n' || b->p[b->n - 1] == '\r' || b->p[b->n - 1] == ' '))
                b->n--;
            if (b->n) {
                fwrite(b->p, 1, b->n, stdout);
                fputc('\n', stdout);
                fflush(stdout);
            }
            picoquic_set_app_stream_ctx(cnx, sid, NULL);
            free(b->p);
            free(b);
            C.inflight--;
        }
        return 0;
    case picoquic_callback_close:
    case picoquic_callback_application_close:
    case picoquic_callback_stateless_reset:
        C.failed = !(C.eof && C.inflight == 0 && C.out == NULL);
        return 0;
    default:
        return 0;
    }
}

static int call_loop_cb(picoquic_quic_t *quic, picoquic_packet_loop_cb_enum mode, void *ctx, void *arg)
{
    (void)quic;
    (void)ctx;
    (void)arg;
    if (C.failed || (C.eof && C.inflight == 0 && C.out == NULL)) {
        C.done = 1;
        return PICOQUIC_NO_ERROR_TERMINATE_PACKET_LOOP;
    }
    if (mode != picoquic_packet_loop_wake_up && !(C.ready && C.out))
        return 0;
    if (!C.ready)
        return 0;
    pthread_mutex_lock(&C.mu);
    line_t *l = C.out;
    C.out = NULL;
    pthread_mutex_unlock(&C.mu);
    /* the list is newest first; send oldest first */
    line_t *r = NULL;
    while (l) {
        line_t *n = l->next;
        l->next = r;
        r = l;
        l = n;
    }
    while (r) {
        line_t *n = r->next;
        uint64_t sid = picoquic_get_next_local_stream_id(C.cnx, 0);
        picoquic_add_to_stream(C.cnx, sid, r->b.p, r->b.n, 1);
        C.inflight++;
        free(r->b.p);
        free(r);
        r = n;
    }
    return 0;
}

/* ---------- main ---------- */

static void maybe_log(picoquic_quic_t *q)
{
    const char *f = getenv("EDGE_LOG"); /* picoquic's text log, for debugging only */
    if (f != NULL && *f) {
        picoquic_set_textlog(q, f);
        picoquic_set_log_level(q, 1);
    }
}

static const char *opt(int argc, char **argv, const char *name)
{
    for (int i = 2; i + 1 < argc; i++)
        if (strcmp(argv[i], name) == 0)
            return argv[i + 1];
    return NULL;
}

int main(int argc, char **argv)
{
#ifdef _WINDOWS
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    const char *mode = argc > 1 ? argv[1] : "", *cert = opt(argc, argv, "--cert"), *key = opt(argc, argv, "--key"),
               *root = opt(argc, argv, "--root");
    int serve = strcmp(mode, "serve") == 0, call = strcmp(mode, "call") == 0;
    if ((!serve && !call) || !cert || !key || !root) {
        fprintf(stderr, "usage: mcp_edge serve|call --cert C --key K --root R ...\n");
        return 2;
    }
    uint64_t now = picoquic_current_time();
    picoquic_packet_loop_param_t lp = {0};
    int ret = 0;
    if (serve) {
        const char *allow = opt(argc, argv, "--allow"), *port = opt(argc, argv, "--port"),
                   *up = opt(argc, argv, "--upstream");
        if (!allow || !port || !up)
            return 2;
        S.upstream = (uint16_t)atoi(up);
        picoquic_quic_t *q = picoquic_create(16, cert, key, NULL, EDGE_ALPN, serve_cb, NULL, NULL, NULL, NULL, now,
                                             NULL, NULL, NULL, 0);
        char *list = strdup(allow);
        if (q == NULL || install_allow_list(q, root, list) != 0) {
            logf_("cannot load certificate, key, root or allow list%s%s", NULL, NULL);
            return 1;
        }
        picoquic_set_default_idle_timeout(q, EDGE_IDLE_MS);
        maybe_log(q);
        lp.local_port = (uint16_t)atoi(port);
        lp.local_af = AF_INET;
        logf_("serving on udp/%s, allow %s", port, allow);
        S.thread = picoquic_start_network_thread(q, &lp, serve_loop_cb, NULL, &ret);
        if (S.thread == NULL)
            return 1;
        while (!S.thread->thread_is_closed)
            edge_sleep_ms(500);
        return S.thread->return_code;
    }
    const char *name = opt(argc, argv, "--server-name"), *to = opt(argc, argv, "--connect");
    char host[128];
    const char *colon = to ? strrchr(to, ':') : NULL;
    if (!name || !colon || (size_t)(colon - to) >= sizeof(host))
        return 2;
    memcpy(host, to, (size_t)(colon - to));
    host[colon - to] = 0;
    struct sockaddr_storage addr;
    if (picoquic_store_text_addr(&addr, host, htons((uint16_t)atoi(colon + 1))) /* expects network order */ != 0)
        return 2;
    int anon = strcmp(cert, "none") == 0; /* only for the no-certificate control */
    picoquic_quic_t *q = picoquic_create(1, anon ? NULL : cert, anon ? NULL : key, root, EDGE_ALPN, NULL, NULL, NULL, NULL, NULL, now, NULL, NULL,
                                         NULL, 0);
    if (q == NULL)
        return 1;
    picoquic_set_default_idle_timeout(q, EDGE_IDLE_MS);
    maybe_log(q);
    C.cnx = picoquic_create_cnx(q, picoquic_null_connection_id, picoquic_null_connection_id,
                                (struct sockaddr *)&addr, now, 0, name, EDGE_ALPN, 1);
    if (C.cnx == NULL)
        return 1;
    picoquic_set_callback(C.cnx, call_cb, NULL);
    picoquic_enable_keep_alive(C.cnx, 0);
    if (picoquic_start_client_cnx(C.cnx) != 0)
        return 1;
    lp.local_af = addr.ss_family;
    C.thread = picoquic_start_network_thread(q, &lp, call_loop_cb, NULL, &ret);
    if (C.thread == NULL)
        return 1;
    char *line = NULL;
    size_t cap = 0;
    for (;;) {
        line_t *l = calloc(1, sizeof(*l));
        int c, ok = 0;
        while ((c = getchar()) != EOF && c != '\n')
            if (c != '\r' && buf_add(&l->b, &(char){(char)c}, 1) == 0)
                ok = 1;
        if (ok) {
            pthread_mutex_lock(&C.mu);
            l->next = C.out;
            C.out = l;
            pthread_mutex_unlock(&C.mu);
            picoquic_wake_up_network_thread(C.thread);
        } else {
            free(l);
        }
        if (c == EOF)
            break;
    }
    free(line);
    (void)cap;
    C.eof = 1;
    picoquic_wake_up_network_thread(C.thread);
    while (!C.done)
        edge_sleep_ms(50);
    if (C.failed)
        logf_("failed: the handshake was refused or the edge went away%s%s", NULL, NULL);
    return C.failed ? 1 : 0;
}
