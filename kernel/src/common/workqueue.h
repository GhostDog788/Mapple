#ifndef MAPPLE_WORKQUEUE_H
#define MAPPLE_WORKQUEUE_H

#include <linux/types.h>
#include <linux/workqueue.h>

int mapple_workqueue_init(void);
bool mapple_workqueue_add(struct work_struct *work);
void mapple_workqueue_cleanup(void);

#endif /* MAPPLE_WORKQUEUE_H */
