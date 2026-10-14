/* Host-only test: real HTTP parser/admission code, injected blocking routes. */
#define _GNU_SOURCE
#include <assert.h>
#include "../src/cr_http.c"
#include "../src/cr_launch.h"
#include "../src/cr_repo_mirror.h"

pthread_mutex_t g_cfg_lock = PTHREAD_MUTEX_INITIALIZER;
cheatrunner_config_t g_cfg;
volatile int g_shutdown_requested;
pthread_mutex_t g_games_cache_lock = PTHREAD_MUTEX_INITIALIZER;
int g_games_scan_busy;
repo_mirror_progress_t g_repo_mirror = { .lock = PTHREAD_MUTEX_INITIALIZER };
pthread_mutex_t g_launch_status_lock = PTHREAD_MUTEX_INITIALIZER;
launch_status_state_t g_launch_status;
long long g_startup_ms;
#define CHEATRUNNER_VERSION "host-test"
void log_msg(const char *fmt, ...) { (void)fmt; }
static int listener_ready;
void notify(const char *fmt, ...) {
  if (strstr(fmt, "Listening on:")) __atomic_store_n(&listener_ready, 1, __ATOMIC_RELEASE);
}
void cr_log(const char *level, const char *tag, const char *fmt, ...) {
  (void)level; (void)tag; (void)fmt;
}
static int shutdown_calls;
void cheatrunner_request_shutdown(int delay_ms) { (void)delay_ms; shutdown_calls++; }
/* Extracted verbatim from cr_api.c by test runner. */
#include "api_handlers.inc"

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static int entered, released;
void http_route(int fd, const char *method, const char *path, const char *query,
                const char *ip, const char *body, size_t len, int gzip) {
  (void)body; (void)len; (void)gzip;
  if (!strcmp(path, "/api/health")) { handle_api_health(fd); return; }
  if (!strcmp(path, "/api/dev/shutdown")) { handle_api_dev_shutdown(fd, method, query, ip); return; }
  if (!strcmp(path, "/block") || !strcmp(path, "/api/cheats/state") || !strcmp(path, "/api/running")) {
    pthread_mutex_lock(&gate);
    entered++; pthread_cond_broadcast(&ready);
    while (!released) pthread_cond_wait(&ready, &gate);
    pthread_mutex_unlock(&gate);
  }
  http_send_json(fd, 200, "{\"ok\":true}");
}
static int start(const char *request, pthread_t *tid) {
  int pair[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  struct timeval tv = { .tv_sec = 2 };
  setsockopt(pair[1], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  http_client_ctx_t *ctx = calloc(1, sizeof(*ctx)); assert(ctx); ctx->fd = pair[0];
  pthread_mutex_lock(&g_active_lock);
  assert(g_active_clients < HTTP_MAX_CONCURRENT);
  g_active_clients++;
  pthread_mutex_unlock(&g_active_lock);
  assert(!pthread_create(tid, NULL, http_client_thread, ctx));
  assert(send(pair[1], request, strlen(request), 0) == (ssize_t)strlen(request));
  return pair[1];
}
static void check(const char *request, const char *expected) {
  pthread_t tid;
  int fd = start(request, &tid);
  char buf[4096]; size_t used = 0;
  for (;;) {
    ssize_t n = recv(fd, buf + used, sizeof(buf) - 1 - used, 0);
    assert(n >= 0);
    if (!n) break;
    used += n; assert(used < sizeof(buf) - 1);
  }
  buf[used] = 0;
  if (!strstr(buf, expected)) { fprintf(stderr, "Expected %s; got %s\n", expected, buf); abort(); }
  close(fd); pthread_join(tid, NULL);
}
#define GET(path) "GET " path " HTTP/1.1\r\nHost: test\r\n\r\n"

static int connect_local(int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0); assert(fd >= 0);
  struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(port),
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr))) { close(fd); return -1; }
  return fd;
}
static void network_check(int port, const char *request, const char *expected) {
  int fd = connect_local(port); assert(fd >= 0);
  struct timeval tv = { .tv_sec = 2 };
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  assert(send(fd, request, strlen(request), 0) == (ssize_t)strlen(request));
  char buf[4096]; size_t used = 0;
  for (;;) {
    ssize_t n = recv(fd, buf + used, sizeof(buf) - 1 - used, 0); assert(n >= 0);
    if (!n) break;
    used += n; assert(used < sizeof(buf) - 1);
  }
  buf[used] = 0;
  if (!strstr(buf, expected)) fprintf(stderr, "Expected %s; got %s\n", expected, buf);
  assert(strstr(buf, expected)); close(fd);
}
static void listener_test(void) {
  /* Port zero lets the kernel allocate an ephemeral port for the real accept loop. */
  g_cfg.http_port = 0;
  pthread_t server; assert(!pthread_create(&server, NULL, http_server_thread, NULL));
  int listener = -1;
  for (int i = 0; i < 100; i++) {
    /* The listen notification runs after listen(); use the shared fd only then. */
    if (__atomic_load_n(&listener_ready, __ATOMIC_ACQUIRE)) { listener = g_http_listen_fd; break; }
    usleep(10000);
  }
  assert(listener >= 0);
  struct sockaddr_in addr; socklen_t size = sizeof(addr);
  assert(!getsockname(listener, (struct sockaddr *)&addr, &size));
  int port = ntohs(addr.sin_port), peers[HTTP_MAX_ROUTES];
  for (int i = 0; i < HTTP_MAX_ROUTES; i++) {
    peers[i] = connect_local(port); assert(peers[i] >= 0);
    assert(send(peers[i], GET("/block"), strlen(GET("/block")), 0) == (ssize_t)strlen(GET("/block")));
  }
  pthread_mutex_lock(&gate);
  while (entered < HTTP_MAX_ROUTES) pthread_cond_wait(&ready, &gate);
  pthread_mutex_unlock(&gate);
  for (int i = 0; i < HTTP_MAX_ROUTES; i++) close(peers[i]);
  sleep(11); /* Closing clients and socket timeouts must not release stuck route slots. */
  network_check(port, GET("/normal"), "HTTP/1.1 503 ");
  network_check(port, GET("/api/health"), "200 OK");
  g_cfg.dev_reload_enabled = 1;
  network_check(port, "POST /api/dev/shutdown HTTP/1.1\r\nContent-Length: 0\r\n\r\n", "shutting down");
  pthread_mutex_lock(&gate); released = 1; pthread_cond_broadcast(&ready); pthread_mutex_unlock(&gate);
  for (int i = 0; i < 100 && cr_http_active_clients(); i++) usleep(10000);
  assert(cr_http_active_clients() == 0);
  network_check(port, GET("/normal"), "200 OK");
  puts("PASS: real TCP accept loop retains recovery access after 20 stuck routes and closed peers; capacity recovers");
  /* Process exit stops the listener, which otherwise intentionally remains in accept(). */
}
int main(int argc, char **argv) {
  (void)argv;
  signal(SIGPIPE, SIG_IGN);
  alarm(30);
  if (argc > 1) { listener_test(); return 0; }

  signal(SIGPIPE, SIG_IGN);
  pthread_t stuck[HTTP_MAX_ROUTES];
  int peers[HTTP_MAX_ROUTES];
  peers[0] = start(GET("/api/cheats/state"), &stuck[0]);
  pthread_mutex_lock(&gate);
  while (entered < 1) pthread_cond_wait(&ready, &gate);
  pthread_mutex_unlock(&gate);
  for (int i = 0; i < 30; i++) check(GET("/api/cheats/state"), "probe_busy");
  check(GET("/api/running"), "probe_busy");
  assert(cr_http_active_clients() == 1);
  for (int i = 1; i < HTTP_MAX_ROUTES; i++) peers[i] = start(GET("/block"), &stuck[i]);
  pthread_mutex_lock(&gate);
  while (entered < HTTP_MAX_ROUTES) pthread_cond_wait(&ready, &gate);
  pthread_mutex_unlock(&gate);
  for (int i = 0; i < HTTP_MAX_ROUTES; i++) close(peers[i]);
  check(GET("/normal"), "too_many_requests");
  check(GET("/api/health"), "\"activeRoutes\":20");
  pthread_mutex_t *locks[] = {&g_games_cache_lock, &g_repo_mirror.lock, &g_launch_status_lock};
  for (unsigned i = 0; i < sizeof(locks)/sizeof(*locks); i++) {
    pthread_mutex_lock(locks[i]);
    check(GET("/api/health"), "\"detailsAvailable\":false");
    pthread_mutex_unlock(locks[i]);
  }
  check(GET("/api/dev/shutdown"), "method_not_allowed");
  pthread_mutex_lock(&g_cfg_lock);
  check("POST /api/dev/shutdown HTTP/1.1\r\nContent-Length: 0\r\n\r\n", "config_busy");
  pthread_mutex_unlock(&g_cfg_lock);
  check("POST /api/dev/shutdown HTTP/1.1\r\nContent-Length: 0\r\n\r\n", "dev_reload_disabled");
  assert(shutdown_calls == 0);
  g_cfg.dev_reload_enabled = 1;
  check("POST /api/dev/shutdown HTTP/1.1\r\nContent-Length: 0\r\n\r\n", "shutting down");
  assert(shutdown_calls == 1);
  assert(cr_http_active_clients() == HTTP_MAX_ROUTES);
  pthread_mutex_lock(&gate); released = 1; pthread_cond_broadcast(&ready); pthread_mutex_unlock(&gate);
  for (int i = 0; i < HTTP_MAX_ROUTES; i++) pthread_join(stuck[i], NULL);
  assert(cr_http_active_clients() == 0);
  check(GET("/api/cheats/state"), "200 OK");
  int routes; long long age; cr_http_work_stats(&routes, &age);
  assert(routes == 0 && age == -1);
  puts("PASS: repeated probes bounded; 20 stuck routes retain health and authorized shutdown; health lock contention; recovery releases capacity");

  /* Partial input cannot occupy the parsing reserve beyond its total deadline. */
  pthread_t slow, slow_body;
  int fd = start("GET /api/health HTTP/1.1\r\n", &slow);
  int body_fd = start("POST /normal HTTP/1.1\r\nContent-Length: 100\r\n\r\nx", &slow_body);
  long long began = http_now_ms();
  for (int i = 0; i < 4; i++) {
    sleep(2);
    assert(send(fd, "x", 1, 0) == 1);
    assert(send(body_fd, "x", 1, 0) == 1);
  }
  char buf[1024];
  struct timeval wait = { .tv_sec = 12 };
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait));
  assert(recv(fd, buf, sizeof(buf), 0) > 0); /* 400 after incomplete headers */
  pthread_join(slow, NULL); close(fd);
  assert(http_now_ms() - began >= 9000 && http_now_ms() - began < 12000);
  setsockopt(body_fd, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait));
  ssize_t n = recv(body_fd, buf, sizeof(buf) - 1, 0);
  assert(n > 0); buf[n] = 0; assert(strstr(buf, "400"));
  pthread_join(slow_body, NULL); close(body_fd);
  assert(cr_http_active_clients() == 0);
  puts("PASS: trickling incomplete headers and bodies expire at total deadline and free reserved capacity");
}
