#include <control/commands/pslist/security.h>

#include <asm/unaligned.h>
#include <linux/capability.h>
#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/namei.h>
#include <linux/path.h>
#include <linux/sched/coredump.h>
#include <linux/sched/mm.h>
#include <linux/seccomp.h>
#include <linux/security.h>
#include <linux/string.h>
#include <linux/uidgid.h>
#include <linux/user_namespace.h>

#define MAPPLE_PSLIST_SECURITY_GROUP_CHUNK 64U

struct mapple_pslist_captured_security {
	struct mapple_pslist_security_record wire;
	const struct cred *cred;
	char *lsm_context;
	u32 lsm_context_length;
};

void mapple_pslist_capture_security_metadata(
	struct mapple_pslist_security_metadata *metadata)
{
	static const struct {
		const char *name;
		const char *path;
	} providers[] = {
		{ "apparmor", "/sys/kernel/security/apparmor" },
		{ "selinux", "/sys/fs/selinux" },
		{ "smack", "/sys/fs/smackfs" },
	};
	const char *provider = NULL;
	struct path path;
	size_t index;
	int lookup_error = 0;
	int ret;

	memset(metadata, 0, sizeof(*metadata));
	for (index = 0; index < ARRAY_SIZE(providers); ++index) {
		ret = kern_path(providers[index].path, LOOKUP_FOLLOW, &path);
		if (ret) {
			if (ret != -ENOENT && ret != -ENOTDIR && !lookup_error)
				lookup_error = -ret;
			continue;
		}
		path_put(&path);
		if (provider) {
			metadata->lsm_provider_status = MAPPLE_PSLIST_DATA_UNAVAILABLE;
			metadata->lsm_provider_error = EINVAL;
			return;
		}
		provider = providers[index].name;
	}
	if (!provider) {
		metadata->lsm_provider_status = MAPPLE_PSLIST_DATA_UNAVAILABLE;
		metadata->lsm_provider_error = lookup_error ? lookup_error : ENOENT;
		return;
	}
	metadata->lsm_provider_length = strlen(provider);
	memcpy(metadata->lsm_provider, provider, metadata->lsm_provider_length);
	metadata->lsm_provider_status = MAPPLE_PSLIST_DATA_AVAILABLE;
}

static void mapple_pslist_encode_record(struct mapple_pslist_stream_record *record,
					u32 size, u16 type, u64 group)
{
	put_unaligned_be32(size, &record->record_size);
	put_unaligned_be16(type, &record->record_type);
	put_unaligned_be64(group, &record->group);
}

static int mapple_pslist_write_object_begin(struct mapple_command_context *context,
					    u64 object_id)
{
	struct mapple_pslist_object_begin begin = { 0 };

	mapple_pslist_encode_record(&begin.record, sizeof(begin),
				    MAPPLE_PSLIST_STREAM_OBJECT_BEGIN,
				    MAPPLE_PSLIST_GROUP_SECURITY);
	put_unaligned_be64(object_id, &begin.object_id);
	put_unaligned_be16(MAPPLE_PSLIST_OBJECT_TASK, &begin.object_type);
	return mapple_command_write(context, &begin, sizeof(begin));
}

static int mapple_pslist_write_object_end(struct mapple_command_context *context,
					  u64 object_id)
{
	struct mapple_pslist_object_end end = { 0 };

	mapple_pslist_encode_record(&end.record, sizeof(end),
				    MAPPLE_PSLIST_STREAM_OBJECT_END,
				    MAPPLE_PSLIST_GROUP_SECURITY);
	put_unaligned_be64(object_id, &end.object_id);
	put_unaligned_be16(MAPPLE_PSLIST_OBJECT_TASK, &end.object_type);
	put_unaligned_be16(MAPPLE_PSLIST_OBJECT_COMPLETE, &end.status);
	put_unaligned_be32(1, &end.record_count);
	return mapple_command_write(context, &end, sizeof(end));
}

static void mapple_pslist_capture_dumpable(struct mapple_pslist_security_record *wire,
					    struct task_struct *task)
{
	struct mm_struct *mm = get_task_mm(task);

	if (!mm) {
		put_unaligned_be16(MAPPLE_PSLIST_DATA_UNAVAILABLE,
				   &wire->dumpable_status);
		put_unaligned_be16(MAPPLE_PSLIST_DUMPABLE_REASON_NO_MM,
				   &wire->dumpable_reason);
		return;
	}
	put_unaligned_be16(MAPPLE_PSLIST_DATA_AVAILABLE, &wire->dumpable_status);
	put_unaligned_be32(get_dumpable(mm), &wire->dumpable);
	mmput(mm);
}

static void mapple_pslist_capture_lsm(struct mapple_pslist_captured_security *captured,
				       struct task_struct *task)
{
	struct mapple_pslist_security_record *wire = &captured->wire;
	u32 secid;
	int ret;

	security_task_getsecid_obj(task, &secid);
	put_unaligned_be32(secid, &wire->lsm_secid);
	ret = security_secid_to_secctx(secid, &captured->lsm_context,
				       &captured->lsm_context_length);
	if (ret) {
		captured->lsm_context = NULL;
		captured->lsm_context_length = 0;
		put_unaligned_be16(MAPPLE_PSLIST_DATA_UNAVAILABLE, &wire->lsm_status);
		put_unaligned_be32(-ret, &wire->lsm_error);
		return;
	}
	put_unaligned_be16(MAPPLE_PSLIST_DATA_AVAILABLE, &wire->lsm_status);
	put_unaligned_be32(captured->lsm_context_length, &wire->lsm_context_length);
}

static void mapple_pslist_capture_credentials(
	struct mapple_pslist_captured_security *captured, struct task_struct *task)
{
	struct mapple_pslist_security_record *wire = &captured->wire;
	const struct cred *cred = get_task_cred(task);

	captured->cred = cred;
	put_unaligned_be32(from_kuid_munged(&init_user_ns, cred->uid), &wire->uid);
	put_unaligned_be32(from_kgid_munged(&init_user_ns, cred->gid), &wire->gid);
	put_unaligned_be32(from_kuid_munged(&init_user_ns, cred->euid),
			   &wire->effective_uid);
	put_unaligned_be32(from_kgid_munged(&init_user_ns, cred->egid),
			   &wire->effective_gid);
	put_unaligned_be32(from_kuid_munged(&init_user_ns, cred->suid), &wire->saved_uid);
	put_unaligned_be32(from_kgid_munged(&init_user_ns, cred->sgid), &wire->saved_gid);
	put_unaligned_be32(from_kuid_munged(&init_user_ns, cred->fsuid),
			   &wire->filesystem_uid);
	put_unaligned_be32(from_kgid_munged(&init_user_ns, cred->fsgid),
			   &wire->filesystem_gid);
	put_unaligned_be32(cred->user_ns->ns.inum, &wire->user_namespace);
	put_unaligned_be32(cred->securebits, &wire->securebits);
	put_unaligned_be32(cred->group_info->ngroups, &wire->supplementary_group_count);
	put_unaligned_be64(cred->cap_inheritable.val, &wire->capability_inheritable);
	put_unaligned_be64(cred->cap_permitted.val, &wire->capability_permitted);
	put_unaligned_be64(cred->cap_effective.val, &wire->capability_effective);
	put_unaligned_be64(cred->cap_bset.val, &wire->capability_bounding);
	put_unaligned_be64(cred->cap_ambient.val, &wire->capability_ambient);
}

static int mapple_pslist_capture_security(struct mapple_pslist_captured_security *captured,
					  struct task_struct *task,
					  const struct mapple_pslist_security_metadata *metadata,
					  u64 object_id)
{
	u64 variable_size;

	mapple_pslist_capture_credentials(captured, task);
	put_unaligned_be64(object_id, &captured->wire.object_id);
	put_unaligned_be32(task_no_new_privs(task), &captured->wire.no_new_privs);
#ifdef CONFIG_SECCOMP
	put_unaligned_be32(READ_ONCE(task->seccomp.mode), &captured->wire.seccomp_mode);
#ifdef CONFIG_SECCOMP_FILTER
	put_unaligned_be32(atomic_read(&task->seccomp.filter_count),
			   &captured->wire.seccomp_filter_count);
#endif
#endif
	mapple_pslist_capture_dumpable(&captured->wire, task);
	mapple_pslist_capture_lsm(captured, task);
	put_unaligned_be16(metadata->lsm_provider_status,
			   &captured->wire.lsm_provider_status);
	put_unaligned_be32(metadata->lsm_provider_error,
			   &captured->wire.lsm_provider_error);
	put_unaligned_be32(metadata->lsm_provider_length,
			   &captured->wire.lsm_provider_length);
	variable_size = (u64)captured->cred->group_info->ngroups * sizeof(__u32) +
			metadata->lsm_provider_length +
			captured->lsm_context_length;
	if (variable_size > U32_MAX - sizeof(captured->wire))
		return -EOVERFLOW;
	mapple_pslist_encode_record(&captured->wire.record,
				    sizeof(captured->wire) + variable_size,
				    MAPPLE_PSLIST_STREAM_SECURITY,
				    MAPPLE_PSLIST_GROUP_SECURITY);
	return 0;
}

static int mapple_pslist_write_supplementary_groups(
	struct mapple_command_context *context, const struct group_info *group_info)
{
	__u32 groups[MAPPLE_PSLIST_SECURITY_GROUP_CHUNK];
	unsigned int index = 0;
	unsigned int chunk_index;
	unsigned int count;
	int ret;

	while (index < group_info->ngroups) {
		count = min_t(unsigned int, MAPPLE_PSLIST_SECURITY_GROUP_CHUNK,
			      group_info->ngroups - index);
		for (chunk_index = 0; chunk_index < count; ++chunk_index)
			put_unaligned_be32(from_kgid_munged(
				&init_user_ns, group_info->gid[index + chunk_index]),
				&groups[chunk_index]);
		ret = mapple_command_write(context, groups, count * sizeof(groups[0]));
		if (ret)
			return ret;
		index += count;
	}
	return 0;
}

int mapple_pslist_write_security_object(struct mapple_command_context *context,
					struct task_struct *task,
					const struct mapple_pslist_security_metadata *metadata,
					u64 object_id,
					u32 *object_count)
{
	struct mapple_pslist_captured_security captured = { 0 };
	int ret;

	ret = mapple_pslist_capture_security(&captured, task, metadata, object_id);
	if (ret)
		goto release;
	ret = mapple_pslist_write_object_begin(context, object_id);
	if (ret)
		goto release;
	++*object_count;
	ret = mapple_command_write(context, &captured.wire, sizeof(captured.wire));
	if (!ret)
		ret = mapple_pslist_write_supplementary_groups(
			context, captured.cred->group_info);
	if (!ret)
		ret = mapple_command_write(context, metadata->lsm_provider,
					   metadata->lsm_provider_length);
	if (!ret)
		ret = mapple_command_write(context, captured.lsm_context,
					   captured.lsm_context_length);
	if (!ret)
		ret = mapple_pslist_write_object_end(context, object_id);

release:
	if (captured.lsm_context)
		security_release_secctx(captured.lsm_context,
					captured.lsm_context_length);
	if (captured.cred)
		put_cred(captured.cred);
	return ret;
}
