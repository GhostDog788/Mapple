#ifndef MAPPLE_COMMAND_PSLIST_SECURITY_H
#define MAPPLE_COMMAND_PSLIST_SECURITY_H

#include <linux/sched.h>
#include <linux/types.h>

#include <control/commands/commands.h>

struct mapple_pslist_security_metadata {
	char lsm_provider[MAPPLE_PSLIST_LSM_PROVIDER_MAX_SIZE];
	u32 lsm_provider_length;
	u32 lsm_provider_error;
	u16 lsm_provider_status;
};

void mapple_pslist_capture_security_metadata(
	struct mapple_pslist_security_metadata *metadata);
int mapple_pslist_write_security_object(struct mapple_command_context *context,
					struct task_struct *task,
					const struct mapple_pslist_security_metadata *metadata,
					u64 object_id,
					u32 *object_count);

#endif /* MAPPLE_COMMAND_PSLIST_SECURITY_H */
