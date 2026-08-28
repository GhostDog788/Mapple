#define _GNU_SOURCE

#include <commands/pslist/security.h>

#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <inttypes.h>
#include <linux/capability.h>
#include <linux/seccomp.h>
#include <linux/securebits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <commands/pslist/json.h>

static __u16 mapple_pslist_security_read_u16(const void *record, size_t offset)
{
	__u16 value;

	memcpy(&value, (const unsigned char *)record + offset, sizeof(value));
	return ntohs(value);
}

static __u32 mapple_pslist_security_read_u32(const void *record, size_t offset)
{
	__u32 value;

	memcpy(&value, (const unsigned char *)record + offset, sizeof(value));
	return ntohl(value);
}

static __u64 mapple_pslist_security_read_u64(const void *record, size_t offset)
{
	__u64 value;

	memcpy(&value, (const unsigned char *)record + offset, sizeof(value));
	return be64toh(value);
}

static int mapple_pslist_security_data_status_valid(__u16 status)
{
	return status >= MAPPLE_PSLIST_DATA_AVAILABLE &&
	       status <= MAPPLE_PSLIST_DATA_UNSUPPORTED;
}

int mapple_pslist_decode_security_record(const void *record, size_t size,
					 struct mapple_pslist_security *security)
{
	const struct mapple_pslist_security_record *wire = record;
	const unsigned char *bytes;
	__u32 group_count;
	__u32 provider_length;
	__u32 context_length;
	__u64 object_id;
	__u64 variable_size;
	size_t index;

	if (size < sizeof(*wire) || security->has_record)
		return EPROTO;
	object_id = mapple_pslist_security_read_u64(
		wire, offsetof(struct mapple_pslist_security_record, object_id));
	if (object_id != security->object_id)
		return EPROTO;
#define SECURITY_U32(field)                                                        \
	security->field = mapple_pslist_security_read_u32(                          \
		wire, offsetof(struct mapple_pslist_security_record, field))
#define SECURITY_U64(field)                                                        \
	security->field = mapple_pslist_security_read_u64(                          \
		wire, offsetof(struct mapple_pslist_security_record, field))
	SECURITY_U32(uid);
	SECURITY_U32(gid);
	SECURITY_U32(effective_uid);
	SECURITY_U32(effective_gid);
	SECURITY_U32(saved_uid);
	SECURITY_U32(saved_gid);
	SECURITY_U32(filesystem_uid);
	SECURITY_U32(filesystem_gid);
	SECURITY_U32(user_namespace);
	SECURITY_U32(securebits);
	SECURITY_U32(no_new_privs);
	SECURITY_U32(seccomp_mode);
	SECURITY_U32(seccomp_filter_count);
	security->dumpable_status = mapple_pslist_security_read_u16(
		wire, offsetof(struct mapple_pslist_security_record, dumpable_status));
	security->dumpable_reason = mapple_pslist_security_read_u16(
		wire, offsetof(struct mapple_pslist_security_record, dumpable_reason));
	security->lsm_status = mapple_pslist_security_read_u16(
		wire, offsetof(struct mapple_pslist_security_record, lsm_status));
	security->lsm_provider_status = mapple_pslist_security_read_u16(
		wire, offsetof(struct mapple_pslist_security_record, lsm_provider_status));
	SECURITY_U32(dumpable);
	security->lsm_error = (__s32)mapple_pslist_security_read_u32(
		wire, offsetof(struct mapple_pslist_security_record, lsm_error));
	SECURITY_U32(lsm_secid);
	security->lsm_provider_error = (__s32)mapple_pslist_security_read_u32(
		wire, offsetof(struct mapple_pslist_security_record, lsm_provider_error));
	group_count = mapple_pslist_security_read_u32(
		wire, offsetof(struct mapple_pslist_security_record,
			       supplementary_group_count));
	provider_length = mapple_pslist_security_read_u32(
		wire, offsetof(struct mapple_pslist_security_record, lsm_provider_length));
	context_length = mapple_pslist_security_read_u32(
		wire, offsetof(struct mapple_pslist_security_record, lsm_context_length));
	SECURITY_U64(capability_inheritable);
	SECURITY_U64(capability_permitted);
	SECURITY_U64(capability_effective);
	SECURITY_U64(capability_bounding);
	SECURITY_U64(capability_ambient);
#undef SECURITY_U64
#undef SECURITY_U32
	variable_size = (__u64)group_count * sizeof(__u32) + provider_length +
			context_length;
	if (wire->reserved || !mapple_pslist_security_data_status_valid(
				       security->dumpable_status) ||
	    !mapple_pslist_security_data_status_valid(security->lsm_status) ||
	    !mapple_pslist_security_data_status_valid(security->lsm_provider_status) ||
	    security->no_new_privs > 1 || security->seccomp_mode > SECCOMP_MODE_FILTER ||
	    security->dumpable_reason > MAPPLE_PSLIST_DUMPABLE_REASON_NO_MM ||
	    group_count > MAPPLE_PSLIST_SECURITY_MAX_SUPPLEMENTARY_GROUPS ||
	    provider_length > MAPPLE_PSLIST_LSM_PROVIDER_MAX_SIZE ||
	    sizeof(*wire) + variable_size != size ||
	    (security->lsm_provider_status == MAPPLE_PSLIST_DATA_AVAILABLE) !=
		    !!provider_length ||
	    (security->lsm_status == MAPPLE_PSLIST_DATA_AVAILABLE) != !!context_length)
		return EPROTO;
	if (security->dumpable_status != MAPPLE_PSLIST_DATA_AVAILABLE &&
	    security->dumpable)
		return EPROTO;
	if ((security->dumpable_status == MAPPLE_PSLIST_DATA_AVAILABLE &&
	     security->dumpable_reason != MAPPLE_PSLIST_DUMPABLE_REASON_NONE) ||
	    (security->dumpable_status != MAPPLE_PSLIST_DATA_AVAILABLE &&
	     security->dumpable_reason == MAPPLE_PSLIST_DUMPABLE_REASON_NONE) ||
	    (security->lsm_provider_status == MAPPLE_PSLIST_DATA_AVAILABLE &&
	     security->lsm_provider_error) ||
	    (security->lsm_status == MAPPLE_PSLIST_DATA_AVAILABLE &&
	     security->lsm_error))
		return EPROTO;
	bytes = (const unsigned char *)record + sizeof(*wire);
	if (group_count) {
		security->supplementary_groups = malloc(
			group_count * sizeof(*security->supplementary_groups));
		if (!security->supplementary_groups)
			return ENOMEM;
		for (index = 0; index < group_count; ++index)
			security->supplementary_groups[index] =
				mapple_pslist_security_read_u32(bytes,
							 index * sizeof(__u32));
		security->supplementary_group_count = group_count;
		bytes += group_count * sizeof(__u32);
	}
	if (provider_length) {
		security->lsm_provider = malloc(provider_length);
		if (!security->lsm_provider)
			return ENOMEM;
		memcpy(security->lsm_provider, bytes, provider_length);
		security->lsm_provider_length = provider_length;
		bytes += provider_length;
	}
	if (context_length) {
		security->lsm_context = malloc(context_length);
		if (!security->lsm_context)
			return ENOMEM;
		memcpy(security->lsm_context, bytes, context_length);
		security->lsm_context_length = context_length;
	}
	security->has_record = 1;
	return 0;
}

void mapple_pslist_free_security(struct mapple_pslist_security *security)
{
	free(security->supplementary_groups);
	free(security->lsm_provider);
	free(security->lsm_context);
	memset(security, 0, sizeof(*security));
}

static const char *mapple_pslist_security_object_status_name(__u16 status)
{
	switch (status) {
	case MAPPLE_PSLIST_OBJECT_COMPLETE: return "complete";
	case MAPPLE_PSLIST_OBJECT_PARTIAL: return "partial";
	case MAPPLE_PSLIST_OBJECT_UNAVAILABLE: return "unavailable";
	case MAPPLE_PSLIST_OBJECT_RACED: return "raced";
	case MAPPLE_PSLIST_OBJECT_TOO_LARGE: return "too_large";
	default: return NULL;
	}
}

static const char *mapple_pslist_security_data_status_name(__u16 status)
{
	switch (status) {
	case MAPPLE_PSLIST_DATA_AVAILABLE: return "available";
	case MAPPLE_PSLIST_DATA_UNAVAILABLE: return "unavailable";
	case MAPPLE_PSLIST_DATA_TOO_LARGE: return "too_large";
	case MAPPLE_PSLIST_DATA_RACED: return "raced";
	case MAPPLE_PSLIST_DATA_UNSUPPORTED: return "unsupported";
	default: return NULL;
	}
}

static const char *mapple_pslist_seccomp_mode_name(__u32 mode)
{
	switch (mode) {
	case SECCOMP_MODE_DISABLED: return "disabled";
	case SECCOMP_MODE_STRICT: return "strict";
	case SECCOMP_MODE_FILTER: return "filter";
	default: return NULL;
	}
}

static const char *mapple_pslist_dumpable_name(__u32 dumpable)
{
	switch (dumpable) {
	case 0: return "SUID_DUMP_DISABLE";
	case 1: return "SUID_DUMP_USER";
	case 2: return "SUID_DUMP_ROOT";
	default: return NULL;
	}
}

static const char *mapple_pslist_dumpable_reason_name(__u16 reason)
{
	switch (reason) {
	case MAPPLE_PSLIST_DUMPABLE_REASON_NONE: return NULL;
	case MAPPLE_PSLIST_DUMPABLE_REASON_NO_MM: return "no_memory_descriptor";
	default: return NULL;
	}
}

static int mapple_pslist_print_capabilities(__u64 mask)
{
	static const char *const names[] = {
		[CAP_CHOWN] = "CAP_CHOWN",
		[CAP_DAC_OVERRIDE] = "CAP_DAC_OVERRIDE",
		[CAP_DAC_READ_SEARCH] = "CAP_DAC_READ_SEARCH",
		[CAP_FOWNER] = "CAP_FOWNER",
		[CAP_FSETID] = "CAP_FSETID",
		[CAP_KILL] = "CAP_KILL",
		[CAP_SETGID] = "CAP_SETGID",
		[CAP_SETUID] = "CAP_SETUID",
		[CAP_SETPCAP] = "CAP_SETPCAP",
		[CAP_LINUX_IMMUTABLE] = "CAP_LINUX_IMMUTABLE",
		[CAP_NET_BIND_SERVICE] = "CAP_NET_BIND_SERVICE",
		[CAP_NET_BROADCAST] = "CAP_NET_BROADCAST",
		[CAP_NET_ADMIN] = "CAP_NET_ADMIN",
		[CAP_NET_RAW] = "CAP_NET_RAW",
		[CAP_IPC_LOCK] = "CAP_IPC_LOCK",
		[CAP_IPC_OWNER] = "CAP_IPC_OWNER",
		[CAP_SYS_MODULE] = "CAP_SYS_MODULE",
		[CAP_SYS_RAWIO] = "CAP_SYS_RAWIO",
		[CAP_SYS_CHROOT] = "CAP_SYS_CHROOT",
		[CAP_SYS_PTRACE] = "CAP_SYS_PTRACE",
		[CAP_SYS_PACCT] = "CAP_SYS_PACCT",
		[CAP_SYS_ADMIN] = "CAP_SYS_ADMIN",
		[CAP_SYS_BOOT] = "CAP_SYS_BOOT",
		[CAP_SYS_NICE] = "CAP_SYS_NICE",
		[CAP_SYS_RESOURCE] = "CAP_SYS_RESOURCE",
		[CAP_SYS_TIME] = "CAP_SYS_TIME",
		[CAP_SYS_TTY_CONFIG] = "CAP_SYS_TTY_CONFIG",
		[CAP_MKNOD] = "CAP_MKNOD",
		[CAP_LEASE] = "CAP_LEASE",
		[CAP_AUDIT_WRITE] = "CAP_AUDIT_WRITE",
		[CAP_AUDIT_CONTROL] = "CAP_AUDIT_CONTROL",
		[CAP_SETFCAP] = "CAP_SETFCAP",
		[CAP_MAC_OVERRIDE] = "CAP_MAC_OVERRIDE",
		[CAP_MAC_ADMIN] = "CAP_MAC_ADMIN",
		[CAP_SYSLOG] = "CAP_SYSLOG",
		[CAP_WAKE_ALARM] = "CAP_WAKE_ALARM",
		[CAP_BLOCK_SUSPEND] = "CAP_BLOCK_SUSPEND",
		[CAP_AUDIT_READ] = "CAP_AUDIT_READ",
		[CAP_PERFMON] = "CAP_PERFMON",
		[CAP_BPF] = "CAP_BPF",
		[CAP_CHECKPOINT_RESTORE] = "CAP_CHECKPOINT_RESTORE",
	};
	int first = 1;
	size_t index;

	if (printf("{\"mask\": \"0x%016" PRIx64 "\", \"names\": [",
		   (uint64_t)mask) < 0)
		return EIO;
	for (index = 0; index < sizeof(names) / sizeof(names[0]); ++index) {
		if (!(mask & (1ULL << index)))
			continue;
		if (printf("%s\"%s\"", first ? "" : ", ",
			   names[index] ? names[index] : "unknown") < 0)
			return EIO;
		first = 0;
	}
	return fputs("]}", stdout) == EOF ? EIO : 0;
}

static int mapple_pslist_print_securebits(__u32 securebits)
{
	static const struct {
		__u32 mask;
		const char *name;
	} bits[] = {
		{ SECBIT_NOROOT, "SECBIT_NOROOT" },
		{ SECBIT_NOROOT_LOCKED, "SECBIT_NOROOT_LOCKED" },
		{ SECBIT_NO_SETUID_FIXUP, "SECBIT_NO_SETUID_FIXUP" },
		{ SECBIT_NO_SETUID_FIXUP_LOCKED, "SECBIT_NO_SETUID_FIXUP_LOCKED" },
		{ SECBIT_KEEP_CAPS, "SECBIT_KEEP_CAPS" },
		{ SECBIT_KEEP_CAPS_LOCKED, "SECBIT_KEEP_CAPS_LOCKED" },
		{ SECBIT_NO_CAP_AMBIENT_RAISE, "SECBIT_NO_CAP_AMBIENT_RAISE" },
		{ SECBIT_NO_CAP_AMBIENT_RAISE_LOCKED,
		  "SECBIT_NO_CAP_AMBIENT_RAISE_LOCKED" },
	};
	int first = 1;
	size_t index;

	if (printf("{\"value\": %u, \"names\": [", securebits) < 0)
		return EIO;
	for (index = 0; index < sizeof(bits) / sizeof(bits[0]); ++index) {
		if (!(securebits & bits[index].mask))
			continue;
		if (printf("%s\"%s\"", first ? "" : ", ", bits[index].name) < 0)
			return EIO;
		first = 0;
	}
	return fputs("]}", stdout) == EOF ? EIO : 0;
}

static int mapple_pslist_print_error(__s32 error)
{
	const char *name;

	if (!error)
		return fputs("null", stdout) == EOF ? EIO : 0;
	name = strerrorname_np(error);
	if (printf("{\"code\": %d, \"name\": ", error) < 0)
		return EIO;
	if (name) {
		if (printf("\"%s\"", name) < 0)
			return EIO;
	} else if (fputs("null", stdout) == EOF) {
		return EIO;
	}
	return putchar('}') == EOF ? EIO : 0;
}

int mapple_pslist_print_json_security(const struct mapple_pslist_security *security,
				       int indent)
{
	const char *dumpable_status = mapple_pslist_security_data_status_name(
		security->dumpable_status);
	const char *lsm_status = mapple_pslist_security_data_status_name(
		security->lsm_status);
	const char *lsm_provider_status = mapple_pslist_security_data_status_name(
		security->lsm_provider_status);
	const char *object_status = mapple_pslist_security_object_status_name(
		security->status);
	const char *seccomp_mode = mapple_pslist_seccomp_mode_name(security->seccomp_mode);
	int first_capability = 1;
	size_t index;
	int ret;

	if (!object_status || !dumpable_status || !lsm_status ||
	    !lsm_provider_status || !seccomp_mode)
		return EPROTO;
	if (printf("{\n%*s\"collection_status\": \"%s\","
		   "\n%*s\"credentials\": {"
		   "\n%*s\"ids_mapped_to\": \"initial_user_namespace\","
		   "\n%*s\"uids\": {\"real\": %u, \"effective\": %u, "
		   "\"saved\": %u, \"filesystem\": %u},"
		   "\n%*s\"gids\": {\"real\": %u, \"effective\": %u, "
		   "\"saved\": %u, \"filesystem\": %u},"
		   "\n%*s\"supplementary_gids\": [",
		   indent + 2, "", object_status, indent + 2, "", indent + 4, "",
		   indent + 4, "", security->uid, security->effective_uid,
		   security->saved_uid, security->filesystem_uid, indent + 4, "",
		   security->gid, security->effective_gid, security->saved_gid,
		   security->filesystem_gid, indent + 4, "") < 0)
		return EIO;
	for (index = 0; index < security->supplementary_group_count; ++index)
		if (printf("%s%u", index ? ", " : "",
			   security->supplementary_groups[index]) < 0)
			return EIO;
	if (printf("],\n%*s\"credential_user_namespace\": {\"inode\": %u}"
		   "\n%*s},\n%*s\"securebits\": ", indent + 4, "",
		   security->user_namespace, indent + 2, "", indent + 2, "") < 0)
		return EIO;
	ret = mapple_pslist_print_securebits(security->securebits);
#define PRINT_CAPABILITY(name, field)                                               \
	do {                                                                          \
		if (!ret && printf("%s\n%*s\"%s\": ",                              \
				   first_capability ? "" : ",", indent + 4, "", (name)) < 0) \
			ret = EIO;                                                       \
		if (!ret) {                                                             \
			ret = mapple_pslist_print_capabilities(security->field);          \
			first_capability = 0;                                             \
		}                                                                       \
	} while (0)
	if (!ret && printf(",\n%*s\"capabilities\": {", indent + 2, "") < 0)
		ret = EIO;
	PRINT_CAPABILITY("inheritable", capability_inheritable);
	PRINT_CAPABILITY("permitted", capability_permitted);
	PRINT_CAPABILITY("effective", capability_effective);
	PRINT_CAPABILITY("bounding", capability_bounding);
	PRINT_CAPABILITY("ambient", capability_ambient);
#undef PRINT_CAPABILITY
	if (!ret && printf("\n%*s},\n%*s\"no_new_privs\": %s,"
			   "\n%*s\"seccomp\": {\"mode\": {\"value\": %u, "
			   "\"name\": \"%s\"}, \"filter_program_count\": %u},"
			   "\n%*s\"dumpable\": {\"collection_status\": \"%s\", "
			   "\"reason\": ", indent + 2, "", indent + 2, "",
			   security->no_new_privs ? "true" : "false", indent + 2, "",
			   security->seccomp_mode, seccomp_mode,
			   security->seccomp_filter_count, indent + 2, "", dumpable_status) < 0)
		ret = EIO;
	if (!ret && security->dumpable_reason == MAPPLE_PSLIST_DUMPABLE_REASON_NONE) {
		if (fputs("null", stdout) == EOF)
			ret = EIO;
	} else if (!ret && printf("\"%s\"",
				  mapple_pslist_dumpable_reason_name(
					  security->dumpable_reason)) < 0) {
		ret = EIO;
	}
	if (!ret && fputs(", \"value\": ", stdout) == EOF)
		ret = EIO;
	if (!ret && security->dumpable_status == MAPPLE_PSLIST_DATA_AVAILABLE) {
		const char *dumpable_name = mapple_pslist_dumpable_name(security->dumpable);

		if (printf("%u, \"name\": ", security->dumpable) < 0)
			ret = EIO;
		if (!ret && dumpable_name && printf("\"%s\"}", dumpable_name) < 0)
			ret = EIO;
		else if (!ret && !dumpable_name && fputs("null}", stdout) == EOF)
			ret = EIO;
	} else if (!ret && fputs("null, \"name\": null}", stdout) == EOF) {
		ret = EIO;
	}
	if (!ret && printf(",\n%*s\"lsm\": {\"collection_status\": \"%s\", "
			   "\"provider\": {\"collection_status\": \"%s\", "
			   "\"name\": ", indent + 2, "", lsm_status,
			   lsm_provider_status) < 0)
		ret = EIO;
	if (!ret && security->lsm_provider)
		ret = mapple_pslist_print_json_bytes(security->lsm_provider,
						      security->lsm_provider_length);
	else if (!ret && fputs("null", stdout) == EOF)
		ret = EIO;
	if (!ret && fputs(", \"error\": ", stdout) == EOF)
		ret = EIO;
	if (!ret)
		ret = mapple_pslist_print_error(security->lsm_provider_error);
	if (!ret && printf("}, \"identity_type\": \"objective\", \"secid\": %u, "
			   "\"context\": ", security->lsm_secid) < 0)
		ret = EIO;
	if (!ret && security->lsm_context)
		ret = mapple_pslist_print_json_bytes(security->lsm_context,
					      security->lsm_context_length);
	else if (!ret && fputs("null", stdout) == EOF)
		ret = EIO;
	if (!ret && fputs(", \"error\": ", stdout) == EOF)
		ret = EIO;
	if (!ret)
		ret = mapple_pslist_print_error(security->lsm_error);
	if (!ret && printf("}\n%*s}", indent, "") < 0)
		ret = EIO;
	return ret;
}
