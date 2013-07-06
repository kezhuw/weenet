#ifndef __WEENET_SCHEDULE_H_
#define __WEENET_SCHEDULE_H_

#include "types.h"

int weenet_init_scheduler(int nthread);

struct weenet_process;
void weenet_schedule_resume(struct weenet_process *p);

#endif
