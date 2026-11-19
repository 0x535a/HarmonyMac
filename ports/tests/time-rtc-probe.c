/* SPDX-License-Identifier: GPL-3.0-only
 * Read RTC only; verify timer delivery without changing time or suspending.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/rtc.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <unistd.h>

static void delivered(int clock)
{
    int fd = timerfd_create(clock, TFD_CLOEXEC | TFD_NONBLOCK);
    assert(fd >= 0);
    assert(fcntl(fd, F_GETFD) & FD_CLOEXEC);
    assert(fcntl(fd, F_GETFL) & O_NONBLOCK);
    struct itimerspec timer = {.it_value={.tv_nsec=20000000}};
    assert(timerfd_settime(fd, 0, &timer, NULL) == 0);
    struct pollfd p = {.fd=fd,.events=POLLIN};
    assert(poll(&p, 1, 1500) == 1 && (p.revents & POLLIN));
    uint64_t count = 0;
    assert(read(fd, &count, sizeof(count)) == sizeof(count) && count >= 1);
    assert(close(fd) == 0);
}

int main(int argc, char **argv)
{
    setbuf(stdout, NULL); alarm(8);
    printf("TIME_RTC_IDENTITY uid=%u gid=%u\n",getuid(),getgid());
    FILE *status=fopen("/proc/self/status","r");
    if(status){char line[256];while(fgets(line,sizeof(line),status))if(!strncmp(line,"CapEff:",7))fputs(line,stdout);fclose(status);}
    int rtc = open("/dev/rtc0", O_RDONLY | O_CLOEXEC);
    struct rtc_time now = {0};
    assert(rtc >= 0 && ioctl(rtc, RTC_RD_TIME, &now) == 0);
    assert(now.tm_year >= 120 && now.tm_year < 200 && now.tm_mon >= 0 && now.tm_mon < 12);
    assert(now.tm_mday >= 1 && now.tm_mday <= 31 && now.tm_hour >= 0 && now.tm_hour < 24);
    close(rtc);
    printf("RTC_READ_ONLY_PASS year=%d month=%d day=%d hardware=virtual_pl031\n",
           now.tm_year+1900, now.tm_mon+1, now.tm_mday);
    delivered(CLOCK_MONOTONIC);
    delivered(CLOCK_REALTIME);
    assert(timerfd_create(127, TFD_CLOEXEC) == -1 && errno == EINVAL);
    int raw = syscall(SYS_timerfd_create, 12, TFD_CLOEXEC);
    printf("POWEROFF_CLOCK_RAW result=%d errno=%d\n", raw, raw < 0 ? errno : 0);
    if (raw >= 0) close(raw);
    if (argc >= 2 && (!strcmp(argv[1], "--compat") || !strcmp(argv[1], "--compat-denied"))) {
        Dl_info info;
        assert(dladdr((void *)timerfd_create, &info) && info.dli_fname &&
               strstr(info.dli_fname, argc>=3?argv[2]:"libaurora-time-alarm.so"));
        printf("TIME_ALARM_LOADED_LIBRARY path=%s\n",info.dli_fname);
        if(!strcmp(argv[1], "--compat-denied")){
            errno=0;int denied=timerfd_create(12,TFD_CLOEXEC|TFD_NONBLOCK);
            printf("TIME_ALARM_PERMISSION result=%d errno=%d\n",denied,errno);
            assert(denied==-1&&errno==EPERM);
            puts("TIME_ALARM_PERMISSION_DENIED_PASS");
        }else{
            delivered(12);
            puts("TIME_ALARM_COMPAT_PASS mapping=12_to_REALTIME_ALARM power_on_from_shutdown=unsupported");
        }
    }
    puts("TIME_STANDARD_TIMERS_PASS");
    return 0;
}
