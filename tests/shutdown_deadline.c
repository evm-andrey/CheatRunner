#define _DEFAULT_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <pthread.h>
#include <sys/wait.h>
#include <time.h>
static int mode;
static int create_thread(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void *), void *arg) {
  if (mode == 5) return EAGAIN;
  return pthread_create(t, attr, fn, arg);
}
#define pthread_create create_thread
#include "../src/cr_shutdown.c"
#undef pthread_create

volatile int g_game_monitor_running = 1;
int g_http_listen_fd = -1;
pthread_mutex_t g_cheat_apply_lock = PTHREAD_MUTEX_INITIALIZER;
void cr_log(const char *level, const char *tag, const char *fmt, ...) {
  (void)level; (void)tag; (void)fmt;
  if (mode == 1) for (;;) pause();
}
void activity_save(void) { if (mode == 2) for (;;) pause(); }
void notifications_save(void) { if (mode == 3) for (;;) pause(); }
int main(void) {
  for (mode = 0; mode < 6; mode++) {
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
      alarm(5); /* A hung test fails with SIGALRM, not the expected SIGKILL. */
      if (mode >= 4) pthread_mutex_lock(&g_cheat_apply_lock);
      cheatrunner_request_shutdown(0);
      for (;;) pause();
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
  }
  puts("PASS: shutdown exits with normal cleanup, stuck logger/activity/notifications/apply, and watchdog creation failure");
}
