#include <control/commands/pslist/task/resources.h>

#include <asm/unaligned.h>
#include <linux/mm.h>
#include <linux/sched/cputime.h>

static void mapple_pslist_capture_cpu(struct mapple_pslist_resources_record *wire,
				       struct task_struct *task)
{
	u64 user_time;
	u64 system_time;

	task_cputime_adjusted(task, &user_time, &system_time);
	put_unaligned_be64(user_time, &wire->cpu_user_time_ns);
	put_unaligned_be64(system_time, &wire->cpu_system_time_ns);
	put_unaligned_be64(READ_ONCE(task->min_flt), &wire->minor_faults);
	put_unaligned_be64(READ_ONCE(task->maj_flt), &wire->major_faults);
	put_unaligned_be64(READ_ONCE(task->nvcsw), &wire->voluntary_context_switches);
	put_unaligned_be64(READ_ONCE(task->nivcsw),
			   &wire->involuntary_context_switches);
	put_unaligned_be32(task_cpu(task), &wire->current_cpu);
	put_unaligned_be32(READ_ONCE(task->policy), &wire->scheduling_policy);
	put_unaligned_be32(READ_ONCE(task->prio), &wire->priority);
	put_unaligned_be32(task_nice(task), &wire->nice);
	put_unaligned_be32(READ_ONCE(task->rt_priority), &wire->realtime_priority);
}

static void mapple_pslist_capture_io(struct mapple_pslist_resources_record *wire,
				      struct task_struct *task)
{
#if defined(CONFIG_TASK_XACCT) && defined(CONFIG_TASK_IO_ACCOUNTING)
	put_unaligned_be16(MAPPLE_PSLIST_DATA_AVAILABLE, &wire->io_status);
	put_unaligned_be64(READ_ONCE(task->ioac.rchar), &wire->io_read_characters);
	put_unaligned_be64(READ_ONCE(task->ioac.wchar), &wire->io_write_characters);
	put_unaligned_be64(READ_ONCE(task->ioac.syscr), &wire->io_read_syscalls);
	put_unaligned_be64(READ_ONCE(task->ioac.syscw), &wire->io_write_syscalls);
	put_unaligned_be64(READ_ONCE(task->ioac.read_bytes),
			   &wire->io_storage_read_bytes);
	put_unaligned_be64(READ_ONCE(task->ioac.write_bytes),
			   &wire->io_storage_write_bytes);
	put_unaligned_be64(READ_ONCE(task->ioac.cancelled_write_bytes),
			   &wire->io_cancelled_write_bytes);
#else
	put_unaligned_be16(MAPPLE_PSLIST_DATA_UNSUPPORTED, &wire->io_status);
#endif
}

static void mapple_pslist_capture_memory(struct mapple_pslist_resources_record *wire,
					  struct task_struct *task)
{
	struct mm_struct *mm;

	put_unaligned_be32(PAGE_SIZE, &wire->page_size_bytes);
	mm = get_task_mm(task);
	if (!mm) {
		put_unaligned_be16(MAPPLE_PSLIST_DATA_UNAVAILABLE, &wire->memory_status);
		put_unaligned_be16(MAPPLE_PSLIST_RESOURCES_MEMORY_REASON_NO_MM,
				   &wire->memory_reason);
		return;
	}
	mmap_read_lock(mm);
	put_unaligned_be64(mm->total_vm, &wire->memory_virtual_pages);
	put_unaligned_be64(get_mm_hiwater_vm(mm), &wire->memory_peak_virtual_pages);
	put_unaligned_be64(mm->locked_vm, &wire->memory_locked_pages);
	put_unaligned_be64(mm->data_vm, &wire->memory_data_pages);
	put_unaligned_be64(mm->stack_vm, &wire->memory_stack_pages);
	put_unaligned_be64(mm->exec_vm, &wire->memory_executable_pages);
	put_unaligned_be64(get_mm_rss(mm), &wire->memory_resident_pages);
	put_unaligned_be64(get_mm_hiwater_rss(mm), &wire->memory_peak_resident_pages);
	put_unaligned_be64(get_mm_counter(mm, MM_ANONPAGES),
			   &wire->memory_anonymous_pages);
	put_unaligned_be64(get_mm_counter(mm, MM_FILEPAGES),
			   &wire->memory_file_pages);
	put_unaligned_be64(get_mm_counter(mm, MM_SHMEMPAGES),
			   &wire->memory_shared_pages);
	put_unaligned_be64(get_mm_counter(mm, MM_SWAPENTS),
			   &wire->memory_swap_entries);
	mmap_read_unlock(mm);
	mmput(mm);
	put_unaligned_be16(MAPPLE_PSLIST_DATA_AVAILABLE, &wire->memory_status);
}

int mapple_pslist_write_resources_object(struct mapple_pslist_writer *writer,
					 struct task_struct *task, u64 object_id)
{
	struct mapple_pslist_resources_record wire = { 0 };
	int ret;

	put_unaligned_be64(object_id, &wire.object_id);
	mapple_pslist_capture_cpu(&wire, task);
	mapple_pslist_capture_io(&wire, task);
	mapple_pslist_capture_memory(&wire, task);
	put_unaligned_be16(MAPPLE_PSLIST_DATA_UNSUPPORTED, &wire.network_status);
	mapple_pslist_encode_record(&wire.record, sizeof(wire), MAPPLE_PSLIST_STREAM_RESOURCES,
				    MAPPLE_PSLIST_GROUP_RESOURCES);
	ret = mapple_pslist_write_object_begin(writer, object_id, MAPPLE_PSLIST_OBJECT_TASK,
					       MAPPLE_PSLIST_GROUP_RESOURCES);
	if (ret)
		return ret;
	ret = mapple_pslist_write(writer, &wire, sizeof(wire));
	if (!ret)
		ret = mapple_pslist_write_object_end(writer, object_id, MAPPLE_PSLIST_OBJECT_TASK,
						     MAPPLE_PSLIST_GROUP_RESOURCES,
						     MAPPLE_PSLIST_OBJECT_COMPLETE, 0, 1);
	return ret;
}
