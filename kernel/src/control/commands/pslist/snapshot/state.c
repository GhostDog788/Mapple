#include <control/commands/pslist/snapshot/state.h>

#include <linux/err.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>

static u64 mapple_pslist_effective_groups(u64 requested)
{
	if (requested & (MAPPLE_PSLIST_GROUP_FDS_DETAILS | MAPPLE_PSLIST_GROUP_NETWORK))
		requested |= MAPPLE_PSLIST_GROUP_FDS;
	if (requested & MAPPLE_PSLIST_GROUP_ELF)
		requested |= MAPPLE_PSLIST_GROUP_MEMORY_MAPS;
	return requested;
}

struct mapple_pslist_snapshot *mapple_pslist_snapshot_create(struct mapple_command_context *command,
							     u64 requested_groups)
{
	struct mapple_pslist_snapshot *snapshot;
	unsigned int group;

	snapshot = kzalloc(sizeof(*snapshot), GFP_KERNEL);
	if (!snapshot)
		return NULL;
	snapshot->writer.command = command;
	snapshot->requested_groups = requested_groups;
	snapshot->effective_groups = mapple_pslist_effective_groups(requested_groups);
	for (group = 0; group < MAPPLE_PSLIST_GROUP_INDEX_COUNT; ++group)
		snapshot->next_object_id[group] = 1;
	hash_init(snapshot->objects);
	mapple_pslist_elf_registry_init(&snapshot->elf_registry);
	if (snapshot->effective_groups & MAPPLE_PSLIST_GROUP_SECURITY)
		mapple_pslist_capture_security_metadata(&snapshot->security_metadata);
	return snapshot;
}

void mapple_pslist_snapshot_release(struct mapple_pslist_snapshot *snapshot)
{
	struct mapple_pslist_snapshot_object *object;
	struct hlist_node *temporary;
	unsigned int bucket;

	hash_for_each_safe(snapshot->objects, bucket, temporary, object, node) {
		hash_del(&object->node);
		if (object->mm)
			mmput(object->mm);
		kfree(object);
	}
	mapple_pslist_elf_registry_release(&snapshot->elf_registry);
	kfree(snapshot);
}

static unsigned long mapple_pslist_object_hash(const void *key, u16 type)
{
	return (unsigned long)key ^ type;
}

static struct mapple_pslist_snapshot_object *
mapple_pslist_find_object(struct mapple_pslist_snapshot *snapshot, const void *key, u16 type)
{
	struct mapple_pslist_snapshot_object *object;
	unsigned long hash = mapple_pslist_object_hash(key, type);

	hash_for_each_possible(snapshot->objects, object, node, hash) {
		if (object->key == key && object->type == type)
			return object;
	}
	return NULL;
}

static struct mapple_pslist_snapshot_object *
mapple_pslist_add_object(struct mapple_pslist_snapshot *snapshot, const void *key, u16 type)
{
	struct mapple_pslist_snapshot_object *object;

	object = kzalloc(sizeof(*object), GFP_KERNEL);
	if (!object)
		return ERR_PTR(-ENOMEM);
	object->key = key;
	object->type = type;
	hash_add(snapshot->objects, &object->node, mapple_pslist_object_hash(key, type));
	return object;
}

u64 mapple_pslist_next_object_id(struct mapple_pslist_snapshot *snapshot,
				 enum mapple_pslist_group_index group)
{
	return snapshot->next_object_id[group]++;
}

u64 mapple_pslist_group_object_id(struct mapple_pslist_snapshot *snapshot,
				  struct mapple_pslist_snapshot_object *object,
				  enum mapple_pslist_group_index group)
{
	if (!object->group_ids[group])
		object->group_ids[group] = mapple_pslist_next_object_id(snapshot, group);
	return object->group_ids[group];
}

struct mapple_pslist_snapshot_object *
mapple_pslist_snapshot_mm(struct mapple_pslist_snapshot *snapshot, struct task_struct *task,
			  bool *is_new)
{
	struct mapple_pslist_snapshot_object *object;
	struct mm_struct *mm = get_task_mm(task);

	*is_new = false;
	if (!mm)
		return NULL;
	object = mapple_pslist_find_object(snapshot, mm, MAPPLE_PSLIST_OBJECT_MM);
	if (object) {
		mmput(mm);
		return object;
	}
	object = mapple_pslist_add_object(snapshot, mm, MAPPLE_PSLIST_OBJECT_MM);
	if (IS_ERR(object)) {
		mmput(mm);
		return object;
	}
	object->mm = mm;
	*is_new = true;
	return object;
}

struct mapple_pslist_snapshot_object *
mapple_pslist_snapshot_files(struct mapple_pslist_snapshot *snapshot, struct task_struct *task,
			     bool *is_new)
{
	struct mapple_pslist_snapshot_object *object;
	struct files_struct *files;

	*is_new = false;
	task_lock(task);
	files = task->files;
	task_unlock(task);
	if (!files)
		return NULL;
	object = mapple_pslist_find_object(snapshot, files, MAPPLE_PSLIST_OBJECT_FILES);
	if (object)
		return object;
	object = mapple_pslist_add_object(snapshot, files, MAPPLE_PSLIST_OBJECT_FILES);
	if (IS_ERR(object))
		return object;
	/* Modules cannot pin files_struct; the FD collector revalidates it under task_lock. */
	object->files = files;
	*is_new = true;
	return object;
}
