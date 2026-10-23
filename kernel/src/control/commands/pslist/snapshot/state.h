#ifndef MAPPLE_COMMAND_PSLIST_SNAPSHOT_STATE_H
#define MAPPLE_COMMAND_PSLIST_SNAPSHOT_STATE_H

#include <linux/hashtable.h>
#include <linux/mm_types.h>

#include <control/commands/pslist/elf/objects.h>
#include <control/commands/pslist/task/security.h>
#include <control/commands/pslist/snapshot/stream.h>

#define MAPPLE_PSLIST_OBJECT_HASH_BITS 8

enum mapple_pslist_group_index {
	MAPPLE_PSLIST_GROUP_INDEX_IDENTITY,
	MAPPLE_PSLIST_GROUP_INDEX_ENVIRONMENT,
	MAPPLE_PSLIST_GROUP_INDEX_MEMORY_MAPS,
	MAPPLE_PSLIST_GROUP_INDEX_SECURITY,
	MAPPLE_PSLIST_GROUP_INDEX_CONTEXT,
	MAPPLE_PSLIST_GROUP_INDEX_RESOURCES,
	MAPPLE_PSLIST_GROUP_INDEX_FDS,
	MAPPLE_PSLIST_GROUP_INDEX_NETWORK,
	MAPPLE_PSLIST_GROUP_INDEX_COUNT,
};

struct mapple_pslist_snapshot_object {
	struct hlist_node node;
	const void *key;
	u64 group_ids[MAPPLE_PSLIST_GROUP_INDEX_COUNT];
	u16 type;
	struct mm_struct *mm;
	struct files_struct *files;
};

struct mapple_pslist_snapshot {
	struct mapple_pslist_writer writer;
	DECLARE_HASHTABLE(objects, MAPPLE_PSLIST_OBJECT_HASH_BITS);
	struct mapple_pslist_security_metadata security_metadata;
	struct mapple_pslist_elf_registry elf_registry;
	u64 requested_groups;
	u64 effective_groups;
	u64 next_object_id[MAPPLE_PSLIST_GROUP_INDEX_COUNT];
};

struct mapple_pslist_snapshot *mapple_pslist_snapshot_create(struct mapple_command_context *command,
							     u64 requested_groups);
void mapple_pslist_snapshot_release(struct mapple_pslist_snapshot *snapshot);
u64 mapple_pslist_next_object_id(struct mapple_pslist_snapshot *snapshot,
				 enum mapple_pslist_group_index group);
u64 mapple_pslist_group_object_id(struct mapple_pslist_snapshot *snapshot,
				  struct mapple_pslist_snapshot_object *object,
				  enum mapple_pslist_group_index group);
struct mapple_pslist_snapshot_object *
mapple_pslist_snapshot_mm(struct mapple_pslist_snapshot *snapshot, struct task_struct *task,
			  bool *is_new);
struct mapple_pslist_snapshot_object *
mapple_pslist_snapshot_files(struct mapple_pslist_snapshot *snapshot, struct task_struct *task,
			     bool *is_new);

#endif
