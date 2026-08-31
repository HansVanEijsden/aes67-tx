/* Measure the PTP hardware clock rate relative to CLOCK_MONOTONIC.
 * Must open /dev/ptpN first so the kernel resolves the magic PHC clockid.
 * Usage: probe_phc <dev> [seconds] */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#define PHC ((clockid_t)0xFFFFFFE3L)
static long long ns(clockid_t c){ struct timespec t; clock_gettime(c,&t); return (long long)t.tv_sec*1000000000LL+t.tv_nsec; }
int main(int argc,char**argv){
  if(argc<2){ fprintf(stderr,"usage: %s <dev> [secs]\n",argv[0]); return 2; }
  int fd=open(argv[1],O_RDWR); if(fd<0){perror("open");return 1;}
  int secs=(argc>2)?atoi(argv[2]):20;
  long long m0=ns(CLOCK_MONOTONIC),p0=ns(PHC);
  usleep(secs*1000000);
  long long m1=ns(CLOCK_MONOTONIC),p1=ns(PHC);
  long long dm=m1-m0,dp=p1-p0;
  double r=(double)dp/(double)dm;
  printf("open=%s mono_adv=%lld phc_adv=%lld phc_rate=%.9f ppm=%+.2f\n",argv[1],dm,dp,r,(r-1.0)*1e6);
  (void)fd;
  return 0;
}
