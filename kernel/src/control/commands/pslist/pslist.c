#include <control/commands/pslist/pslist.h>

#include <asm/unaligned.h>
#include <linux/errno.h>
#include <linux/pid_namespace.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/string.h>

#define MAPPLE_PSLIST_INITIAL_CAPACITY 64U

static void mapple_pslist_fill_record(struct mapple_pslist_record *record, struct task_struct *task)
{
	memset(record, 0, sizeof(*record));
	put_unaligned_be32(task_tgid_nr_ns(task, &init_pid_ns), &record->pid);
	put_unaligned_be32(task_ppid_nr_ns(task, &init_pid_ns), &record->ppid);
	record->state = task_state_to_char(task);
	get_task_comm(record->name, task);
}

static int mapple_pslist_snapshot(struct mapple_pslist_record **records_out, size_t *count_out)
{
	struct mapple_pslist_record *records;
	struct task_struct *task;
	size_t capacity = MAPPLE_PSLIST_INITIAL_CAPACITY;
	size_t count;
	bool full;

	BUILD_BUG_ON(MAPPLE_PSLIST_NAME_SIZE != TASK_COMM_LEN);
	for (;;) {
		records = kvmalloc_array(capacity, sizeof(*records), GFP_KERNEL);
		if (!records)
			return -ENOMEM;

		count = 0;
		full = false;
		rcu_read_lock();
		for_each_process(task) {
			if (count == capacity) {
				full = true;
				break;
			}
			mapple_pslist_fill_record(&records[count], task);
			count++;
		}
		rcu_read_unlock();

		if (!full) {
			*records_out = records;
			*count_out = count;
			return 0;
		}
		kvfree(records);
		if (capacity > SIZE_MAX / 2)
			return -EOVERFLOW;
		capacity *= 2;
	}
}

int mapple_handle_pslist(struct mapple_command_context *context)
{
	struct mapple_pslist_record *records;
	size_t count;
	size_t response_size;
	int ret;

	if (context->remaining_payload)
		return -EMSGSIZE;
	ret = mapple_pslist_snapshot(&records, &count);
	if (ret)
		return ret;
	response_size = count * sizeof(*records);
	ret = mapple_command_write_response_header(context, MAPPLE_COMMAND_PSLIST, 0, response_size);
	if (!ret && response_size)
		ret = mapple_command_write(context, records, response_size);

	kvfree(records);
	return ret;
}
