#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "cr_log.h"
#include "cr_activity.h"
#include "cr_notifications.h"
#include "cr_shutdown.h"

volatile int g_shutdown_requested = 0;
extern volatile int g_game_monitor_running;
extern int g_http_listen_fd;
/* Held by cr_cheats.c during a cheat apply/disable's writes. */
extern pthread_mutex_t g_cheat_apply_lock;

static void
close_fd_safe(int *fd) {
  if (!fd || *fd < 0) {
    return;
  }
  shutdown(*fd, SHUT_RDWR);
  close(*fd);
  *fd = -1;
}

void
cheatrunner_close_servers(void) {
  close_fd_safe(&g_http_listen_fd);
}

/* Wait briefly for an in-flight apply, then kill. Never block: a stuck apply (hung mdbg/ptrace)
 * must not stop shutdown from reaching SIGKILL. */
static void
kill_after_no_apply_in_flight(void) {
  for (int waited_ms = 0; waited_ms < 2000; waited_ms += 50) {
    if (pthread_mutex_trylock(&g_cheat_apply_lock) == 0) {
      break;
    }
    usleep(50 * 1000);
  }
  kill(getpid(), SIGKILL);
  _exit(0);
}

static void *
shutdown_deadline_thread(void *arg) {
  /* Cleanup/logging can themselves block. Enforce the shutdown deadline from
   * a separate thread that never takes application locks or performs file I/O. */
  int delay_ms = (int)(intptr_t)arg;
  struct timespec remaining = {
    .tv_sec = (delay_ms + 2500) / 1000,
    .tv_nsec = ((delay_ms + 2500) % 1000) * 1000000L
  };
  while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR) {}
  kill(getpid(), SIGKILL);
  _exit(0);
  return NULL;
}

static void *
delayed_shutdown_thread(void *arg) {
  int delay_ms = arg ? *(int *)arg : 700;
  free(arg);
  if (delay_ms < 0) {
    delay_ms = 0;
  }
  usleep((useconds_t)delay_ms * 1000);
  cr_log("info", "dev", "shutdown requested; closing CheatRunner");
  g_shutdown_requested = 1;
  g_game_monitor_running = 0;
  cr_log("info", "dev", "closing HTTP socket");
  cheatrunner_close_servers();
  activity_save();
  notifications_save();
  cr_log("info", "dev", "CheatRunner exiting for reload");
  /* Guaranteed exit: this always reaches SIGKILL/_exit, so a shutdown request never leaves CheatRunner running. */
  kill_after_no_apply_in_flight();
  return NULL;
}

void
cheatrunner_request_shutdown(int delay_ms) {
  if (delay_ms < 0 || delay_ms > 10000) delay_ms = 700;
  pthread_t watchdog;
  if (pthread_create(&watchdog, NULL, shutdown_deadline_thread, (void *)(intptr_t)delay_ms) != 0) {
    /* Do not enter potentially blocking cleanup without an exit deadline. */
    kill_after_no_apply_in_flight();
    return;
  }
  pthread_detach(watchdog);
  int *p = malloc(sizeof(*p));
  if (!p) {
    g_shutdown_requested = 1;
    g_game_monitor_running = 0;
    cheatrunner_close_servers();
    kill_after_no_apply_in_flight();
    return;
  }
  *p = delay_ms;
  pthread_t t;
  if (pthread_create(&t, NULL, delayed_shutdown_thread, p) == 0) {
    pthread_detach(t);
    return;
  }
  free(p);
  g_shutdown_requested = 1;
  g_game_monitor_running = 0;
  cheatrunner_close_servers();
  kill_after_no_apply_in_flight();
}
