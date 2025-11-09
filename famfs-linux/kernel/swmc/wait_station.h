#ifndef _POPCORN_WAIT_STATION_H_
#define _POPCORN_WAIT_STATION_H_

#include <linux/completion.h>
#include <linux/atomic.h>

#define MAX_WAIT_STATIONS 65536
#define WAIT_STATION_THRESHOLD 52428 // 0.8 of MAX_WAIT_STATIONS

struct wait_station {
	int id; // wait station ID
	pid_t pid;
	volatile void *private;
	struct page *async_page; // indicate whether the wait station is for async transaction
	struct completion pendings;
	atomic_t pendings_count;
};

struct task_struct;

struct wait_station *get_wait_station_multiple(struct task_struct *tsk, int count);
static inline struct wait_station *get_wait_station(struct task_struct *tsk)
{
	return get_wait_station_multiple(tsk, 1);
}
struct wait_station *wait_station(int id);
void put_wait_station(struct wait_station *ws);
void *wait_at_station(struct wait_station *ws);
#endif
