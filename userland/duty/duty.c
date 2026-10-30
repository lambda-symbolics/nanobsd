/* duty ON_MS OFF_MS SECONDS: busy for ON_MS, sleep OFF_MS, for SECONDS (docs/cpu-accounting.org) */
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
int main(int c,char**v){double on=atof(v[1])/1000,off=atof(v[2])/1000,end=now()+atof(v[3]);
 while(now()<end){double s=now();while(now()-s<on);usleep(off*1e6);}return 0;}
