/*
 * unlicense vboyko 2022
 * gcc ./myclock.c -O2 -lrt -Wall -Wextra -std=c99 -pedantic -o myclock
 *
 * ./myclock -a | mawk -W interactive '{print strftime("%c", $1);}' | \
 *      while read -r MYTIME; do
 *     echo -ne "\r"
 *     echo -n "${MYTIME}"
 *     xsetroot -name "🐿 $(batstat)% $MYTIME"
 * done
 * TODO add step (2ns or 1s 2ns etc)
 * TODO add utility which would stream xsetroot from stdin instead of calling
 *      process each time
 * TODO add framebuffer frontend
 * TODO make it deliverable after stop
 */
#define  _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t done = 0;

static void handler(int sig) {
    (void)sig;
    done = 1;
}

static int myitoa_abs_10(uint64_t d, char *buf, int precision, int zero)
{
    char tmp[128];
    int i, j, n = 0;

    do
        tmp[n++] = d % 10 + '0';
    while (d /= 10);

    i = 0;
    if (precision >= 0) {
      if ((i = (n - precision)) < 0)
         i = 0;
      if (!zero)
        for (; i < n && tmp[i] == '0'; ++i);
    }
    for (j = 0; i < n; ++j)
        buf[j] = tmp[--n];
    return j;
}

static void mywriteall(int fd, const void *buf, size_t sz)
{
    size_t written = 0;

    do {
       ssize_t n = write(fd, (const char *)buf + written, sz - written);
       if (n > 0)
          written += n;
    } while (written < sz);
}

static uint64_t myprecise_u64(uint64_t d, int src_precision, int dst_precision)
{
   while (src_precision > dst_precision) { d /= 10; --src_precision; }
   while (src_precision < dst_precision) { d *= 10; ++src_precision; }
   return d;
}

static void fatal(int rc, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   vfprintf(stderr, fmt, ap);
   va_end(ap);
   fputc('\n', stderr);
   exit(rc);
}

int main(int argc, char **argv) {
    char buf[124];
    const char *arg;
    int stop, n;
    sigset_t mask;
    timer_t timerid;
    siginfo_t si = {0};
    struct sigaction sa = {0};
    struct sigevent sev = {0};
    struct itimerspec timer = {0};
    int flag_absolute = 0;
    int arg_interval = 0;
    int precision = 0;
    int flag_zeroes = 0;
    int flag_show_clicks = 0;
    uint64_t click = 0, click_count = 0;
    uint64_t isec = 0, insec = 0;
    uint64_t sec = 0, nsec = 0;

    (void)argc;

    while((arg = *++argv))
    {
        stop = 0;
        if (*arg == '-')
        {
          while (!stop && *++arg)
          {
            switch (*arg)
            {
              case 'c':
                  flag_show_clicks = 1;
                  break;
              case 'z':
                  flag_zeroes = 1;
                  break;
              case 'a':
                  flag_absolute = 1;
                  break;
              case 's':
                  if (!*++argv || 1 != sscanf(*argv, "%"SCNu64, &sec))
                    fatal(1, "-s: invalid argument");
                  stop = 1;
                  break;
              case 't':
                  if (!*++argv || sscanf(*argv, "%"SCNu64"%n",
                                    &click_count, &n) < 0 || (*argv)[n])
                  {
                    fatal(1, "-t: invalid argument");
                  }
                  stop = 1;
                  break;
              default:
                fatal(1, "invalid option: %c", *arg);
            }
          }
          continue;
        }

        n = 0;
        arg_interval = (sscanf(arg, "%"SCNu64"%n", &isec, &n) > 0);
        arg += n;
        if (*arg == '.')
          ++arg;

        n = 0;

        /* I do not like floats */
        if (sscanf(arg, "%n%"SCNu64"%n%n%*[0-9]%n", &n, &insec,
                                          &precision, &n, &n) > 0)
        {
            arg_interval = 1;
        }
        arg += n;

        if (*arg)
          fatal(1, "invalid param: '%s', '%s' cannot be parsed", *argv, arg);
        if (!arg_interval)
          fatal(1, "invalid param: %s", *argv);
    }

    if (arg_interval) {
        insec = myprecise_u64(insec, precision, 9);
    } else {
        isec = 1;
    }

    /* TODO */
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGRTMIN, &sa, NULL) < 0)
        fatal(1, "sigaction: %s", strerror(errno));

    sigemptyset(&mask);
    sigaddset(&mask, SIGRTMIN);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0)
        fatal(1, "sigprocmask: %s", strerror(errno));

    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGRTMIN;
    sev.sigev_value.sival_ptr = &timerid;
    if (timer_create(CLOCK_REALTIME, &sev, &timerid) < 0)
        fatal(1, "timer_create: %s", strerror(errno));

    timer.it_interval.tv_sec = isec;
    timer.it_interval.tv_nsec = insec;
    timer.it_value.tv_sec = isec;
    timer.it_value.tv_nsec = insec;
    if (timer_settime(timerid, 0, &timer, NULL) < 0)
        fatal(1, "timer_settime: %s", strerror(errno));

    if (flag_absolute)
      sec += time(NULL);

    do {
        sigwaitinfo(&mask, &si);
        ++click;

        if (flag_show_clicks)
        {
          n = myitoa_abs_10(click, buf, -1, 0);
          buf[n++] = '\n';
          mywriteall(1, buf, n);
          continue;
        }

        sec += isec;
        if (insec)
        {
          nsec += insec;
          if (nsec > 999999999UL)
          {
            ++sec;
            nsec -= 1000000000UL;
          }
        }

        n = myitoa_abs_10(sec, buf, -1, 0);
        if (insec)
        {
          buf[n++] = '.';
          n += myitoa_abs_10(nsec, buf + n, precision, flag_zeroes);
          if (buf[n - 1] == '.')
            --n;
        }

        buf[n++] = '\n';
        mywriteall(1, buf, n);
    } while (click != click_count);

    return 0;
}
