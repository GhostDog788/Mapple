#include <common/workqueue.h>

#include <linux/errno.h>
#include <linux/workqueue.h>

static struct workqueue_struct *mapple_workqueue;

int mapple_workqueue_init(void)
{
	mapple_workqueue = alloc_workqueue("mapple-cleanup", 0, 0);
	return mapple_workqueue ? 0 : -ENOMEM;
}

bool mapple_workqueue_add(struct work_struct *work) { return queue_work(mapple_workqueue, work); }

void mapple_workqueue_cleanup(void)
{
	if (!mapple_workqueue)
		return;
	destroy_workqueue(mapple_workqueue);
	mapple_workqueue = NULL;
}
