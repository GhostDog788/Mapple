package server

import (
	"encoding/hex"
	"fmt"
	"net"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/GhostDog788/Mapple/user/api"
	"github.com/GhostDog788/Mapple/user/server/internal/moduleclient"
	"github.com/GhostDog788/Mapple/user/server/internal/moduleclient/wire"
)

// This file is the server-side "final data" layer for pslist. Its shape
// deliberately mirrors pslist-v8.json, the reference C client's own output
// (schema_version + snapshot + tasks{processes,kthreads,unmatched_threads}
// + objects{identity,environment,security,context,resources,fds,network,
// mapped_files,memory_maps,elf}) -- v8 is main's actual, already-vetted
// design, not a legacy format to diverge from. Every view function below is
// explicit and field-by-field, matching v8's own field names/nesting/
// raw-vs-decoded choices field by field rather than re-deriving a "cleaner"
// shape from scratch, so every choice is traceable back to what v8 itself
// does. Timestamps are the one deliberate, confirmed departure: ISO-8601
// instead of v8's {seconds,nanoseconds} strings.

// --- small shared formatters ---

// strU64/strI64 render a potentially-large wire value as a JSON string
// (matching v8's own convention for inode/size/byte-counter/id fields) to
// avoid float64 precision loss in any generic JSON consumer -- small,
// inherently-bounded fields (uid/gid/device numbers/mode/priorities/counts)
// stay plain JSON numbers, exactly as v8 leaves them.
func strU64(v uint64) string { return strconv.FormatUint(v, 10) }
func strI64(v int64) string  { return strconv.FormatInt(v, 10) }

// hexRaw renders an address with no zero-padding (memory_maps start/end),
// hexPad32/hexPad64 zero-pad to the field's full width (matching v8's own
// per-field choices -- inconsistent between the two styles, but consistently
// reproduced here field by field).
func hexRaw(v uint64) string   { return fmt.Sprintf("0x%x", v) }
func hexPad32(v uint32) string { return fmt.Sprintf("0x%08x", v) }
func hexPad64(v uint64) string { return fmt.Sprintf("0x%016x", v) }

// isoNanos formats an epoch-nanosecond timestamp as UTC ISO-8601 -- the one
// deliberate, explicitly-confirmed departure from v8 (which uses a raw
// {seconds,nanoseconds} string pair for the same fields).
func isoNanos(ns int64) string {
	return time.Unix(0, ns).UTC().Format(time.RFC3339)
}

func isoSecNsec(sec int64, nsec uint32) string {
	return time.Unix(sec, int64(nsec)).UTC().Format(time.RFC3339)
}

// flagBit is one named bit in a bitmask field.
type flagBit struct {
	mask uint64
	name string
}

// flagNames renders a bitmask as the sorted list of its set flag names,
// appending an "unknown_0x..." entry for any bits none of the known names
// cover, so nothing is ever silently dropped for being unrecognized.
func flagNames(value uint64, bits []flagBit) []string {
	names := []string{}
	remaining := value
	for _, b := range bits {
		if b.mask != 0 && value&b.mask == b.mask {
			names = append(names, b.name)
			remaining &^= b.mask
		}
	}
	if remaining != 0 {
		names = append(names, fmt.Sprintf("unknown_%s", hexRaw(remaining)))
	}
	sort.Strings(names)
	return names
}

func idKey(id uint64) string { return strconv.FormatUint(id, 10) }

// --- enum name tables (matching v8's own decoded values field-by-field) ---

func taskStateName(state byte) string {
	switch state {
	case 'R':
		return "running"
	case 'S':
		return "sleeping"
	case 'D':
		return "disk_sleep"
	case 'T':
		return "stopped"
	case 't':
		return "tracing_stop"
	case 'X', 'x':
		return "dead"
	case 'Z':
		return "zombie"
	case 'P':
		return "parked"
	case 'I':
		return "idle"
	default:
		return fmt.Sprintf("unknown_%q", string(state))
	}
}

func objectStatusName(s wire.ObjectStatus) string {
	switch s {
	case wire.ObjectComplete:
		return "complete"
	case wire.ObjectPartial:
		return "partial"
	case wire.ObjectUnavailable:
		return "unavailable"
	case wire.ObjectRaced:
		return "raced"
	case wire.ObjectTooLarge:
		return "too_large"
	default:
		return fmt.Sprintf("unknown_%d", s)
	}
}

func dataStatusName(s wire.DataStatus) string {
	switch s {
	case wire.DataAvailable:
		return "available"
	case wire.DataUnavailable:
		return "unavailable"
	case wire.DataTooLarge:
		return "too_large"
	case wire.DataRaced:
		return "raced"
	case wire.DataUnsupported:
		return "unsupported"
	default:
		return fmt.Sprintf("unknown_%d", s)
	}
}

func commandLineStatusName(s wire.CommandLineStatus) string {
	switch s {
	case wire.CommandLineAvailable:
		return "available"
	case wire.CommandLineEmpty:
		return "empty"
	case wire.CommandLineUnavailable:
		return "unavailable"
	case wire.CommandLineTooLarge:
		return "too_large"
	default:
		return fmt.Sprintf("unknown_%d", s)
	}
}

func executableStatusName(s wire.ExecutableStatus) string {
	switch s {
	case wire.ExecutableAvailable:
		return "available"
	case wire.ExecutableNone:
		return "none"
	case wire.ExecutableUnavailable:
		return "unavailable"
	default:
		return fmt.Sprintf("unknown_%d", s)
	}
}

func environmentStatusName(s wire.EnvironmentStatus) string {
	switch s {
	case wire.EnvironmentAvailable:
		return "available"
	case wire.EnvironmentEmpty:
		return "empty"
	case wire.EnvironmentUnavailable:
		return "unavailable"
	case wire.EnvironmentTooLarge:
		return "too_large"
	case wire.EnvironmentRaced:
		return "raced"
	default:
		return fmt.Sprintf("unknown_%d", s)
	}
}

func memoryReasonName(r wire.MemoryReason) string {
	switch r {
	case wire.MemoryReasonNone:
		return "none"
	case wire.MemoryReasonNoMM:
		return "no_mm"
	default:
		return fmt.Sprintf("unknown_%d", r)
	}
}

func schedulingPolicyName(p uint32) string {
	switch p {
	case 0:
		return "SCHED_OTHER"
	case 1:
		return "SCHED_FIFO"
	case 2:
		return "SCHED_RR"
	case 3:
		return "SCHED_BATCH"
	case 5:
		return "SCHED_IDLE"
	case 6:
		return "SCHED_DEADLINE"
	default:
		return fmt.Sprintf("unknown_%d", p)
	}
}

func seccompModeName(m uint32) string {
	switch m {
	case 0:
		return "disabled"
	case 1:
		return "strict"
	case 2:
		return "filter"
	default:
		return fmt.Sprintf("unknown_%d", m)
	}
}

func dumpableName(v uint32) string {
	switch v {
	case 0:
		return "SUID_DUMP_DISABLE"
	case 1:
		return "SUID_DUMP_USER"
	case 2:
		return "SUID_DUMP_ROOT"
	default:
		return fmt.Sprintf("unknown_%d", v)
	}
}

func mappingTypeName(t wire.MappingType) string {
	switch t {
	case wire.MappingAnonymous:
		return "anonymous"
	case wire.MappingFile:
		return "file"
	case wire.MappingSpecial:
		return "special"
	default:
		return fmt.Sprintf("unknown_%d", t)
	}
}

// elfObjectTypeName/elfMachineName/elfBindingName/elfSymbolTypeName/
// elfVisibilityName are standard ELF header constants (elf.h). Unlike
// os_abi/abi_version (which v8 leaves as bare numbers, no name table
// exists for them there), these get a decoded name *alongside* the raw
// value -- matching v8's object_type/object_type_value and
// machine/machine_value pairs exactly.
func elfObjectTypeName(t uint16) string {
	switch t {
	case 0:
		return "none"
	case 1:
		return "relocatable"
	case 2:
		return "executable"
	case 3:
		return "shared_object"
	case 4:
		return "core"
	default:
		return fmt.Sprintf("unknown_%d", t)
	}
}

func elfMachineName(m uint16) string {
	switch m {
	case 3:
		return "386"
	case 8:
		return "mips"
	case 40:
		return "arm"
	case 62:
		return "x86_64"
	case 183:
		return "aarch64"
	case 243:
		return "riscv"
	default:
		return fmt.Sprintf("unknown_%d", m)
	}
}

func elfClassName(c uint8) string {
	switch c {
	case 1:
		return "ELF32"
	case 2:
		return "ELF64"
	default:
		return fmt.Sprintf("unknown_%d", c)
	}
}

func elfDataEncodingName(c uint8) string {
	switch c {
	case 1:
		return "little_endian"
	case 2:
		return "big_endian"
	default:
		return fmt.Sprintf("unknown_%d", c)
	}
}

func elfBindingName(b uint8) string {
	switch b {
	case 0:
		return "local"
	case 1:
		return "global"
	case 2:
		return "weak"
	default:
		return fmt.Sprintf("unknown_%d", b)
	}
}

func elfSymbolTypeName(t uint8) string {
	switch t {
	case 0:
		return "notype"
	case 1:
		return "object"
	case 2:
		return "func"
	case 3:
		return "section"
	case 4:
		return "file"
	case 6:
		return "tls"
	default:
		return fmt.Sprintf("unknown_%d", t)
	}
}

func elfVisibilityName(v uint8) string {
	switch v {
	case 0:
		return "default"
	case 1:
		return "internal"
	case 2:
		return "hidden"
	case 3:
		return "protected"
	default:
		return fmt.Sprintf("unknown_%d", v)
	}
}

func fdKindName(k wire.FDKind) string {
	switch k {
	case wire.FDRegular:
		return "regular_file"
	case wire.FDDirectory:
		return "directory"
	case wire.FDCharacter:
		return "character_device"
	case wire.FDBlock:
		return "block_device"
	case wire.FDPipe:
		return "pipe"
	case wire.FDSocket:
		return "socket"
	case wire.FDSymlink:
		return "symlink"
	case wire.FDAnonInode:
		return "anon_inode"
	case wire.FDOther:
		return "other"
	default:
		return fmt.Sprintf("unknown_%d", k)
	}
}

// socketEndpointTypeName is the one place a socket-family value gets a
// decoded name alongside its raw value (type/type_value) -- v8 leaves the
// network object's own family/type/protocol/socket_state/protocol_state/
// shutdown as bare numbers.
func socketEndpointTypeName(t wire.SocketEndpointType) string {
	switch t {
	case wire.SocketEndpointNone:
		return "none"
	case wire.SocketEndpointIPv4:
		return "ipv4"
	case wire.SocketEndpointIPv6:
		return "ipv6"
	case wire.SocketEndpointUnix:
		return "unix"
	case wire.SocketEndpointNetlink:
		return "netlink"
	default:
		return fmt.Sprintf("unknown_%d", t)
	}
}

func socketEndpointStatusName(s wire.SocketEndpointStatus) string {
	switch s {
	case wire.SocketEndpointAvailable:
		return "available"
	case wire.SocketEndpointUnavailable:
		return "unavailable"
	case wire.SocketEndpointNotConnected:
		return "not_connected"
	case wire.SocketEndpointUnsupported:
		return "unsupported"
	default:
		return fmt.Sprintf("unknown_%d", s)
	}
}

func openAccessMode(value uint32) string {
	switch value & 0x3 {
	case 1:
		return "write"
	case 2:
		return "read_write"
	default:
		return "read"
	}
}

var openFlagBits = []flagBit{
	{0o100, "O_CREAT"}, {0o200, "O_EXCL"}, {0o400, "O_NOCTTY"}, {0o1000, "O_TRUNC"},
	{0o2000, "O_APPEND"}, {0o4000, "O_NONBLOCK"}, {0o10000, "O_DSYNC"}, {0o20000, "O_ASYNC"},
	{0o40000, "O_DIRECT"}, {0o100000, "O_LARGEFILE"}, {0o200000, "O_DIRECTORY"},
	{0o400000, "O_NOFOLLOW"}, {0o1000000, "O_NOATIME"}, {0o2000000, "O_CLOEXEC"},
	{0o10000000, "O_PATH"}, {0o20000000, "O_TMPFILE"},
}

var namespaceBits = []flagBit{
	{1 << 0, "user"}, {1 << 1, "uts"}, {1 << 2, "ipc"}, {1 << 3, "mount"},
	{1 << 4, "pid_active"}, {1 << 5, "pid_children"}, {1 << 6, "network"},
	{1 << 7, "cgroup"}, {1 << 8, "time"}, {1 << 9, "time_children"},
}

var secbits = []flagBit{
	{1 << 0, "SECBIT_NOROOT"}, {1 << 1, "SECBIT_NOROOT_LOCKED"},
	{1 << 2, "SECBIT_NO_SETUID_FIXUP"}, {1 << 3, "SECBIT_NO_SETUID_FIXUP_LOCKED"},
	{1 << 4, "SECBIT_KEEP_CAPS"}, {1 << 5, "SECBIT_KEEP_CAPS_LOCKED"},
	{1 << 6, "SECBIT_NO_CAP_AMBIENT_RAISE"}, {1 << 7, "SECBIT_NO_CAP_AMBIENT_RAISE_LOCKED"},
}

// capabilityBits mirrors linux/capability.h's CAP_* constants.
var capabilityBits = []flagBit{
	{1 << 0, "CAP_CHOWN"}, {1 << 1, "CAP_DAC_OVERRIDE"}, {1 << 2, "CAP_DAC_READ_SEARCH"},
	{1 << 3, "CAP_FOWNER"}, {1 << 4, "CAP_FSETID"}, {1 << 5, "CAP_KILL"},
	{1 << 6, "CAP_SETGID"}, {1 << 7, "CAP_SETUID"}, {1 << 8, "CAP_SETPCAP"},
	{1 << 9, "CAP_LINUX_IMMUTABLE"}, {1 << 10, "CAP_NET_BIND_SERVICE"}, {1 << 11, "CAP_NET_BROADCAST"},
	{1 << 12, "CAP_NET_ADMIN"}, {1 << 13, "CAP_NET_RAW"}, {1 << 14, "CAP_IPC_LOCK"},
	{1 << 15, "CAP_IPC_OWNER"}, {1 << 16, "CAP_SYS_MODULE"}, {1 << 17, "CAP_SYS_RAWIO"},
	{1 << 18, "CAP_SYS_CHROOT"}, {1 << 19, "CAP_SYS_PTRACE"}, {1 << 20, "CAP_SYS_PACCT"},
	{1 << 21, "CAP_SYS_ADMIN"}, {1 << 22, "CAP_SYS_BOOT"}, {1 << 23, "CAP_SYS_NICE"},
	{1 << 24, "CAP_SYS_RESOURCE"}, {1 << 25, "CAP_SYS_TIME"}, {1 << 26, "CAP_SYS_TTY_CONFIG"},
	{1 << 27, "CAP_MKNOD"}, {1 << 28, "CAP_LEASE"}, {1 << 29, "CAP_AUDIT_WRITE"},
	{1 << 30, "CAP_AUDIT_CONTROL"}, {1 << 31, "CAP_SETFCAP"}, {1 << 32, "CAP_MAC_OVERRIDE"},
	{1 << 33, "CAP_MAC_ADMIN"}, {1 << 34, "CAP_SYSLOG"}, {1 << 35, "CAP_WAKE_ALARM"},
	{1 << 36, "CAP_BLOCK_SUSPEND"}, {1 << 37, "CAP_AUDIT_READ"}, {1 << 38, "CAP_PERFMON"},
	{1 << 39, "CAP_BPF"}, {1 << 40, "CAP_CHECKPOINT_RESTORE"},
}

var executableFlagBits = []flagBit{
	{uint64(wire.ExecutableFlagDeleted), "deleted"},
	{uint64(wire.ExecutableFlagPathUnavailable), "path_unavailable"},
	{uint64(wire.ExecutableFlagMetadataUnavailable), "metadata_unavailable"},
}

// vmFlagNameBits renders the "exotic" VM_* bits v8 reports under
// mappings[].vm_flags, in v8's own lowercase names -- everything else
// (the base rwx+shared bits -> permissions/sharing, the MAY* bits ->
// permission_limits/may_share) is pulled out into its own field instead of
// living in this catch-all array.
var vmFlagNameBits = []flagBit{
	{1 << 8, "growsdown"}, {1 << 9, "uffd_missing"}, {1 << 10, "pfnmap"},
	{1 << 12, "uffd_wp"}, {1 << 13, "locked"}, {1 << 14, "io"},
	{1 << 15, "seq_read"}, {1 << 16, "rand_read"}, {1 << 17, "dontcopy"},
	{1 << 18, "dontexpand"}, {1 << 19, "lockonfault"}, {1 << 20, "account"},
	{1 << 21, "noreserve"}, {1 << 22, "hugetlb"}, {1 << 23, "sync"},
	{1 << 25, "wipeonfork"}, {1 << 26, "dontdump"}, {1 << 27, "soft_dirty"},
	{1 << 28, "mixedmap"}, {1 << 29, "hugepage"}, {1 << 30, "nohugepage"},
	{1 << 31, "mergeable"},
}

const (
	vmFlagRead     = 1 << 0
	vmFlagWrite    = 1 << 1
	vmFlagExec     = 1 << 2
	vmFlagShared   = 1 << 3
	vmFlagMayRead  = 1 << 4
	vmFlagMayWrite = 1 << 5
	vmFlagMayExec  = 1 << 6
	vmFlagMayShare = 1 << 7
)

// --- socket address / elf string decoding ---

func decodeSocketAddress(epType wire.SocketEndpointType, raw []byte, abstract bool) string {
	switch epType {
	case wire.SocketEndpointIPv4, wire.SocketEndpointIPv6:
		if len(raw) == 4 || len(raw) == 16 {
			return net.IP(raw).String()
		}
	case wire.SocketEndpointUnix:
		if abstract {
			return "@" + strings.TrimSuffix(strings.TrimPrefix(string(raw), "\x00"), "\x00")
		}
		return strings.TrimRight(string(raw), "\x00")
	}
	if len(raw) == 0 {
		return ""
	}
	return hex.EncodeToString(raw)
}

func decodeELFStringValue(kind wire.ELFStringKind, raw []byte) string {
	if kind == wire.ELFStringBuildID {
		return hex.EncodeToString(raw)
	}
	return string(raw)
}

// splitArgv splits a NUL-delimited argv block (Identity.CommandLine) into
// its individual arguments, dropping the trailing empty element left by
// the final NUL.
func splitArgv(commandLine string) []string {
	if commandLine == "" {
		return []string{}
	}
	parts := strings.Split(commandLine, "\x00")
	if len(parts) > 0 && parts[len(parts)-1] == "" {
		parts = parts[:len(parts)-1]
	}
	return parts
}

func dumpableReasonName(r uint16) string {
	switch r {
	case 0:
		return "none"
	case 1:
		return "no_mm"
	default:
		return fmt.Sprintf("unknown_%d", r)
	}
}

// errorOrNil renders a wire error code the way v8 does throughout: null
// when there's no error, the raw errno number (never a decoded name --
// v8 never decodes an *_error field anywhere) when there is one.
func errorOrNil(code uint32) interface{} {
	if code == 0 {
		return nil
	}
	return code
}

// --- per-group view builders ---
//
// Each function mirrors pslist-v8.json's own field names, nesting and
// raw-vs-decoded choices for that group, field by field -- v8 is main's
// actual, already-vetted design. ISO-8601 timestamps are the one
// deliberate, confirmed departure from v8's raw {seconds,nanoseconds}
// pairs.

const initialUserNamespaceInode = 4026531837

func idsMappedTo(userNamespace uint32) string {
	if uint64(userNamespace) == initialUserNamespaceInode {
		return "initial_user_namespace"
	}
	return "non_initial_user_namespace"
}

// statusOrFlag derives a granular sub-status (path_status/metadata_status)
// from the wire's single overall ExecutableStatus plus its own
// unavailable-flag bit, since the wire protocol tracks one combined status
// where v8's schema tracks three independently.
func statusOrFlag(overall wire.ExecutableStatus, flagUnavailable bool) string {
	if overall != wire.ExecutableAvailable {
		return executableStatusName(overall)
	}
	if flagUnavailable {
		return "unavailable"
	}
	return "available"
}

func viewIdentity(o *moduleclient.IdentityObject) map[string]interface{} {
	m := map[string]interface{}{
		"status":      objectStatusName(o.Status),
		"argv_status": commandLineStatusName(o.CommandLineStatus),
		"argv":        []string{},
	}
	if o.CommandLineStatus == wire.CommandLineAvailable || o.CommandLineStatus == wire.CommandLineEmpty {
		m["argv"] = splitArgv(o.CommandLine)
	}

	pathUnavailable := o.ExecutableFlags&wire.ExecutableFlagPathUnavailable != 0
	metadataUnavailable := o.ExecutableFlags&wire.ExecutableFlagMetadataUnavailable != 0
	deleted := o.ExecutableFlags&wire.ExecutableFlagDeleted != 0

	executable := map[string]interface{}{
		"status":          executableStatusName(o.ExecutableStatus),
		"path_status":     statusOrFlag(o.ExecutableStatus, pathUnavailable),
		"metadata_status": statusOrFlag(o.ExecutableStatus, metadataUnavailable),
		"deleted":         deleted,
	}
	if o.ExecutableStatus == wire.ExecutableAvailable && !pathUnavailable {
		executable["path"] = o.ExecutablePath
	}
	if o.ExecutableStatus == wire.ExecutableAvailable && !metadataUnavailable {
		executable["device_major"] = o.ExecutableDeviceMajor
		executable["device_minor"] = o.ExecutableDeviceMinor
		executable["inode"] = strU64(o.ExecutableInode)
		executable["mode"] = o.ExecutableMode
		executable["size"] = strU64(o.ExecutableSize)
		executable["mtime_ns"] = isoNanos(o.ExecutableMtimeNS)
		executable["ctime_ns"] = isoNanos(o.ExecutableCtimeNS)
		executable["filesystem_magic"] = hexPad64(o.ExecutableFilesystemMagic)
	}
	m["executable"] = executable
	return m
}

func viewSecurity(o *moduleclient.SecurityObject) map[string]interface{} {
	capEntry := func(mask uint64) map[string]interface{} {
		return map[string]interface{}{"mask": hexPad64(mask), "names": flagNames(mask, capabilityBits)}
	}

	dumpable := map[string]interface{}{"collection_status": dataStatusName(o.DumpableStatus)}
	if o.DumpableStatus == wire.DataAvailable {
		dumpable["reason"] = nil
		dumpable["value"] = o.Dumpable
		dumpable["name"] = dumpableName(o.Dumpable)
	} else {
		dumpable["value"] = nil
		dumpable["name"] = nil
		if o.DumpableReason != 0 {
			dumpable["reason"] = dumpableReasonName(o.DumpableReason)
		} else {
			dumpable["reason"] = nil
		}
	}

	provider := map[string]interface{}{"collection_status": dataStatusName(o.LSMProviderStatus)}
	if o.LSMProviderStatus == wire.DataAvailable {
		provider["name"] = o.LSMProvider
		provider["error"] = nil
	} else {
		provider["name"] = nil
		provider["error"] = errorOrNil(o.LSMProviderError)
	}
	lsm := map[string]interface{}{
		"collection_status": dataStatusName(o.LSMStatus),
		"provider":          provider,
	}
	if o.LSMStatus == wire.DataAvailable {
		lsm["identity_type"] = "objective"
		lsm["secid"] = o.LSMSecID
		lsm["context"] = string(o.LSMContext)
		lsm["error"] = nil
	} else {
		lsm["identity_type"] = nil
		lsm["secid"] = nil
		lsm["context"] = nil
		lsm["error"] = errorOrNil(o.LSMError)
	}

	return map[string]interface{}{
		"collection_status": objectStatusName(o.Status),
		"credentials": map[string]interface{}{
			"ids_mapped_to":      idsMappedTo(o.UserNamespace),
			"uids":               map[string]interface{}{"real": o.UID, "effective": o.EffectiveUID, "saved": o.SavedUID, "filesystem": o.FilesystemUID},
			"gids":               map[string]interface{}{"real": o.GID, "effective": o.EffectiveGID, "saved": o.SavedGID, "filesystem": o.FilesystemGID},
			"supplementary_gids": o.SupplementaryGroups,
			"credential_user_namespace": map[string]interface{}{
				"inode": o.UserNamespace,
			},
		},
		"securebits": map[string]interface{}{
			"value": o.Securebits,
			"names": flagNames(uint64(o.Securebits), secbits),
		},
		"capabilities": map[string]interface{}{
			"inheritable": capEntry(o.CapabilityInheritable),
			"permitted":   capEntry(o.CapabilityPermitted),
			"effective":   capEntry(o.CapabilityEffective),
			"bounding":    capEntry(o.CapabilityBounding),
			"ambient":     capEntry(o.CapabilityAmbient),
		},
		"no_new_privs": o.NoNewPrivs != 0,
		"seccomp": map[string]interface{}{
			"mode":                 map[string]interface{}{"value": o.SeccompMode, "name": seccompModeName(o.SeccompMode)},
			"filter_program_count": o.SeccompFilterCount,
		},
		"dumpable": dumpable,
		"lsm":      lsm,
	}
}

func viewContext(o *moduleclient.ContextObject) map[string]interface{} {
	pathField := func(status wire.DataStatus, path string, errCode uint32) map[string]interface{} {
		f := map[string]interface{}{"collection_status": dataStatusName(status)}
		if status == wire.DataAvailable {
			f["path"] = path
			f["error"] = nil
		} else {
			f["path"] = nil
			f["error"] = errorOrNil(errCode)
		}
		return f
	}
	nsField := func(available bool, inode uint32) map[string]interface{} {
		if available {
			return map[string]interface{}{"collection_status": "available", "inode": inode}
		}
		return map[string]interface{}{"collection_status": "unsupported", "inode": nil}
	}

	availMask := flagNames(uint64(o.NamespaceAvailableMask), namespaceBits)
	avail := func(name string) bool {
		for _, a := range availMask {
			if a == name {
				return true
			}
		}
		return false
	}

	cgroup := map[string]interface{}{
		"collection_status": dataStatusName(o.CgroupStatus),
	}
	if o.CgroupStatus == wire.DataAvailable {
		cgroup["scope"] = "default_hierarchy"
		cgroup["id"] = strU64(o.CgroupID)
		cgroup["inode"] = strU64(o.CgroupInode)
		cgroup["hierarchy_id"] = o.CgroupHierarchyID
		cgroup["error"] = nil
	} else {
		cgroup["scope"] = nil
		cgroup["id"] = nil
		cgroup["inode"] = nil
		cgroup["hierarchy_id"] = nil
		cgroup["error"] = errorOrNil(o.CgroupError)
	}
	cgroup["host_path"] = pathField(o.CgroupHostPathStatus, o.CgroupHostPath, o.CgroupHostPathError)
	cgroup["namespace_path"] = pathField(o.CgroupNamespacePathStatus, o.CgroupNamespacePath, o.CgroupNamespacePathError)

	return map[string]interface{}{
		"collection_status": objectStatusName(o.Status),
		"filesystem": map[string]interface{}{
			"cwd":  pathField(o.CWDStatus, o.CWD, o.CWDError),
			"root": pathField(o.RootStatus, o.Root, o.RootError),
		},
		"namespaces": map[string]interface{}{
			"user":          nsField(avail("user"), o.UserNamespace),
			"uts":           nsField(avail("uts"), o.UTSNamespace),
			"ipc":           nsField(avail("ipc"), o.IPCNamespace),
			"mount":         nsField(avail("mount"), 0),
			"pid_active":    nsField(avail("pid_active"), o.PIDNamespaceActive),
			"pid_children":  nsField(avail("pid_children"), o.PIDNamespaceChildren),
			"network":       nsField(avail("network"), o.NetworkNamespace),
			"cgroup":        nsField(avail("cgroup"), o.CgroupNamespace),
			"time":          nsField(avail("time"), o.TimeNamespace),
			"time_children": nsField(avail("time_children"), o.TimeNamespaceChildren),
		},
		"cgroup": cgroup,
	}
}

func viewResources(o *moduleclient.ResourcesObject) map[string]interface{} {
	memory := map[string]interface{}{"collection_status": dataStatusName(o.MemoryStatus)}
	if o.MemoryStatus == wire.DataAvailable {
		memory["error"] = nil
		if o.MemoryReason != wire.MemoryReasonNone {
			memory["reason"] = memoryReasonName(o.MemoryReason)
		} else {
			memory["reason"] = nil
		}
		memory["page_size_bytes"] = o.PageSizeBytes
		memory["pages"] = map[string]interface{}{
			"virtual":       strU64(o.MemoryVirtualPages),
			"peak_virtual":  strU64(o.MemoryPeakVirtualPages),
			"locked":        strU64(o.MemoryLockedPages),
			"data":          strU64(o.MemoryDataPages),
			"stack":         strU64(o.MemoryStackPages),
			"executable":    strU64(o.MemoryExecutablePages),
			"resident":      strU64(o.MemoryResidentPages),
			"peak_resident": strU64(o.MemoryPeakResidentPages),
			"anonymous":     strU64(o.MemoryAnonymousPages),
			"file":          strU64(o.MemoryFilePages),
			"shared":        strU64(o.MemorySharedPages),
			"swap_entries":  strU64(o.MemorySwapEntries),
		}
	} else {
		memory["error"] = errorOrNil(o.MemoryError)
		memory["reason"] = memoryReasonName(o.MemoryReason)
		memory["page_size_bytes"] = o.PageSizeBytes
		memory["pages"] = nil
	}

	io := map[string]interface{}{"collection_status": dataStatusName(o.IOStatus)}
	if o.IOStatus == wire.DataAvailable {
		io["error"] = nil
		io["characters"] = map[string]interface{}{"read": strU64(o.IOReadCharacters), "written": strU64(o.IOWriteCharacters)}
		io["syscalls"] = map[string]interface{}{"read": strU64(o.IOReadSyscalls), "write": strU64(o.IOWriteSyscalls)}
		io["storage_bytes"] = map[string]interface{}{
			"read": strU64(o.IOStorageReadBytes), "written": strU64(o.IOStorageWriteBytes), "cancelled_write": strU64(o.IOCancelledWriteBytes),
		}
	} else {
		io["error"] = errorOrNil(o.IOError)
		io["characters"] = nil
		io["syscalls"] = nil
		io["storage_bytes"] = nil
	}

	network := map[string]interface{}{"collection_status": dataStatusName(o.NetworkStatus)}
	if o.NetworkStatus != wire.DataAvailable {
		network["reason"] = "fresh_snapshot_has_no_per_task_network_accounting"
	} else {
		network["reason"] = nil
	}
	network["error"] = errorOrNil(o.NetworkError)

	return map[string]interface{}{
		"collection_status": objectStatusName(o.Status),
		"cpu": map[string]interface{}{
			"user_time_ns":   strU64(o.CPUUserTimeNS),
			"system_time_ns": strU64(o.CPUSystemTimeNS),
			"faults":         map[string]interface{}{"minor": strU64(o.MinorFaults), "major": strU64(o.MajorFaults)},
			"context_switches": map[string]interface{}{
				"voluntary": strU64(o.VoluntaryContextSwitches), "involuntary": strU64(o.InvoluntaryContextSwitches),
			},
			"scheduler": map[string]interface{}{
				"current_cpu":       o.CurrentCPU,
				"policy":            schedulingPolicyName(o.SchedulingPolicy),
				"priority":          o.Priority,
				"nice":              o.Nice,
				"realtime_priority": o.RealtimePriority,
			},
		},
		"memory":  memory,
		"io":      io,
		"network": network,
	}
}

func viewEnvironment(o *moduleclient.EnvironmentObject) map[string]interface{} {
	block := map[string]interface{}{"collection_status": environmentStatusName(o.AvailabilityStatus)}
	if o.AvailabilityStatus == wire.EnvironmentAvailable {
		entries := make([]map[string]interface{}, 0, o.EntryCount)
		for _, entry := range o.Entries() {
			name, value, ok := strings.Cut(entry, "=")
			if !ok {
				continue
			}
			entries = append(entries, map[string]interface{}{"name": name, "value": value})
		}
		block["byte_length"] = o.ByteLength
		block["entry_count"] = o.EntryCount
		block["terminated"] = o.Flags&0x1 != 0
		block["error"] = nil
		block["entries"] = entries
	} else {
		block["byte_length"] = nil
		block["entry_count"] = nil
		block["terminated"] = nil
		block["error"] = errorOrNil(o.Error)
		block["entries"] = []map[string]interface{}{}
	}
	return map[string]interface{}{
		"collection_status": objectStatusName(o.Status),
		"block":             block,
	}
}

func viewFilesObject(o *moduleclient.FilesObject) map[string]interface{} {
	fds := make([]map[string]interface{}, 0, len(o.FDs))
	for _, entry := range o.FDs {
		fds = append(fds, viewFDEntry(entry))
	}
	return map[string]interface{}{
		"status": objectStatusName(o.Status),
		"fds":    fds,
	}
}

func viewFDEntry(e *moduleclient.FDEntry) map[string]interface{} {
	m := map[string]interface{}{
		"fd":               e.FD.FD,
		"kind":             fdKindName(e.FD.Kind),
		"descriptor_flags": e.FD.DescriptorFlags,
		"open_flags": map[string]interface{}{
			"value":  e.FD.OpenFlags,
			"access": openAccessMode(e.FD.OpenFlags),
			"names":  flagNames(uint64(e.FD.OpenFlags&^0x3), openFlagBits),
		},
		"path_status": dataStatusName(e.FD.PathStatus),
		"path_error":  errorOrNil(e.FD.PathError),
	}
	if e.FD.PathStatus == wire.DataAvailable {
		m["path"] = e.FD.Path
	} else {
		m["path"] = nil
	}

	identity := map[string]interface{}{"status": dataStatusName(e.FD.IdentityStatus)}
	if e.FD.IdentityStatus == wire.DataAvailable {
		identity["error"] = nil
		identity["device_major"] = e.FD.DeviceMajor
		identity["device_minor"] = e.FD.DeviceMinor
		identity["inode"] = strU64(e.FD.Inode)
		identity["mount_id"] = strU64(e.FD.MountID)
	} else {
		identity["error"] = errorOrNil(e.FD.IdentityError)
		identity["device_major"] = nil
		identity["device_minor"] = nil
		identity["inode"] = nil
		identity["mount_id"] = nil
	}
	m["identity"] = identity

	if e.Details != nil {
		m["details"] = viewFDDetails(e.Details, e.FD.DeviceMajor, e.FD.DeviceMinor, e.FD.Inode, e.FD.MountID)
	}
	return m
}

func viewFDDetails(d *wire.FDDetails, deviceMajor, deviceMinor uint32, inode, mountID uint64) map[string]interface{} {
	m := map[string]interface{}{
		"file_mode": hexPad64(d.FileMode),
		"position":  strI64(d.Position),
		"version":   strU64(d.Version),
		"credentials": map[string]interface{}{
			"uid": d.CredentialUID, "gid": d.CredentialGID,
			"effective_uid": d.CredentialEffectiveUID, "effective_gid": d.CredentialEffectiveGID,
			"saved_uid": d.CredentialSavedUID, "saved_gid": d.CredentialSavedGID,
			"filesystem_uid": d.CredentialFilesystemUID, "filesystem_gid": d.CredentialFilesystemGID,
		},
		"async_owner": map[string]interface{}{
			"pid": d.AsyncOwnerPID, "type": d.AsyncOwnerType, "signal": d.AsyncSignal,
			"uid": d.AsyncOwnerUID, "effective_uid": d.AsyncOwnerEffectiveUID,
		},
	}
	if d.StatStatus == wire.DataAvailable {
		m["stat"] = viewFDStat(d.Stat, deviceMajor, deviceMinor, inode, mountID)
	} else {
		m["stat"] = map[string]interface{}{"status": dataStatusName(d.StatStatus), "error": errorOrNil(d.StatError)}
	}
	fdinfo := map[string]interface{}{"status": dataStatusName(d.FDInfoStatus)}
	if d.FDInfoStatus == wire.DataAvailable {
		fdinfo["error"] = nil
		// parse_status/parsed reflect a structured fdinfo parser (e.g. for
		// eventfd counters, epoll targets, pipe sizes) that doesn't exist
		// yet -- raw is always captured, "unrecognized" is honest about
		// what isn't implemented rather than faking a parse.
		fdinfo["parse_status"] = "unrecognized"
		fdinfo["parsed"] = nil
		fdinfo["raw"] = d.FDInfo
	} else {
		fdinfo["error"] = errorOrNil(d.FDInfoError)
		fdinfo["parse_status"] = nil
		fdinfo["parsed"] = nil
		fdinfo["raw"] = nil
	}
	m["fdinfo"] = fdinfo
	return m
}

func viewFDStat(s wire.FDStat, deviceMajor, deviceMinor uint32, inode, mountID uint64) map[string]interface{} {
	return map[string]interface{}{
		"status":                     "available",
		"error":                      nil,
		"result_mask":                s.ResultMask,
		"mode":                       s.Mode,
		"link_count":                 s.LinkCount,
		"block_size":                 s.BlockSize,
		"uid":                        s.UID,
		"gid":                        s.GID,
		"device_major":               deviceMajor,
		"device_minor":               deviceMinor,
		"special_device_major":       s.SpecialDeviceMajor,
		"special_device_minor":       s.SpecialDeviceMinor,
		"direct_io_memory_alignment": s.DirectIOMemoryAlignment,
		"direct_io_offset_alignment": s.DirectIOOffsetAlignment,
		"inode":                      strU64(inode),
		"size":                       strI64(s.Size),
		"blocks":                     strU64(s.Blocks),
		"attributes":                 hexPad64(s.Attributes),
		"attributes_mask":            hexPad64(s.AttributesMask),
		"mount_id":                   strU64(mountID),
		"change_cookie":              strU64(s.ChangeCookie),
		"filesystem_magic":           hexPad64(s.FilesystemMagic),
		"access_time":                isoSecNsec(s.AccessTimeSec, s.AccessTimeNsec),
		"birth_time":                 isoSecNsec(s.BirthTimeSec, s.BirthTimeNsec),
		"change_time":                isoSecNsec(s.ChangeTimeSec, s.ChangeTimeNsec),
		"modification_time":          isoSecNsec(s.ModificationTimeSec, s.ModificationTimeNsec),
	}
}

func viewNetworkObject(o *moduleclient.FilesObject) map[string]interface{} {
	sockets := make([]map[string]interface{}, 0)
	for _, entry := range o.FDs {
		if entry.Network == nil {
			continue
		}
		sockets = append(sockets, map[string]interface{}{
			"fd":      entry.FD.FD,
			"network": viewNetwork(entry.Network),
		})
	}
	return map[string]interface{}{
		"status":  objectStatusName(o.Status),
		"sockets": sockets,
	}
}

// viewNetwork leaves family/type/protocol/socket_state/protocol_state/
// shutdown as bare wire numbers (matching v8 exactly) -- only the socket
// endpoint's own "type" gets a decoded name alongside its raw type_value.
func viewNetwork(n *wire.Network) map[string]interface{} {
	return map[string]interface{}{
		"family":            n.Family,
		"type":              n.SocketType,
		"protocol":          n.Protocol,
		"socket_state":      n.SocketState,
		"protocol_state":    n.ProtocolState,
		"shutdown":          n.Shutdown,
		"error":             errorOrNil(uint32(n.Error)),
		"soft_error":        errorOrNil(uint32(n.SoftError)),
		"bound_device":      n.BoundDevice,
		"mark":              n.Mark,
		"priority":          n.Priority,
		"uid":               n.UID,
		"network_namespace": n.NetworkNamespace,
		"receive_buffer":    n.ReceiveBuffer,
		"send_buffer":       n.SendBuffer,
		"receive_queued":    n.ReceiveQueued,
		"send_queued":       n.SendQueued,
		"drops":             n.Drops,
		"local":             viewSocketEndpoint(n.Local),
		"peer":              viewSocketEndpoint(n.Peer),
	}
}

func viewSocketEndpoint(e wire.SocketEndpoint) map[string]interface{} {
	abstract := e.Flags&wire.SocketEndpointFlagAbstract != 0
	m := map[string]interface{}{
		"status":         socketEndpointStatusName(e.Status),
		"status_value":   e.Status,
		"error":          errorOrNil(uint32(e.Error)),
		"type":           socketEndpointTypeName(e.Type),
		"type_value":     e.Type,
		"port":           e.Port,
		"flags":          e.Flags,
		"scope_id":       e.ScopeID,
		"flow_info":      e.FlowInfo,
		"netlink_pid":    e.NetlinkPID,
		"netlink_groups": e.NetlinkGroups,
	}
	if e.Status == wire.SocketEndpointAvailable && len(e.Address) > 0 {
		m["address"] = decodeSocketAddress(e.Type, e.Address, abstract)
	} else {
		m["address"] = nil
	}
	return m
}

// mappedFileKey identifies a real underlying file by device+inode -- the
// same identity the kernel's own ELF registry dedupes by (see elf.c).
type mappedFileKey struct {
	deviceMajor, deviceMinor uint32
	inode                    uint64
}

// mappedFilesCatalog is a session-scoped, deduplicated pool of mapped
// files, mirroring v8's own objects.mapped_files: today's memory_maps
// design inlined every file's identity (inode/size/mtime/ctime/
// filesystem_magic) into *every* mapping record, so the same shared
// library's metadata repeated once per process that mapped it -- the same
// class of bug the ELF symbol catalog fixed, just for file identity
// instead of symbol names.
type mappedFilesCatalog struct {
	index   map[mappedFileKey]uint64
	entries map[uint64]map[string]interface{}
	elfBack map[uint64][]uint64
	next    uint64
}

func newMappedFilesCatalog() *mappedFilesCatalog {
	return &mappedFilesCatalog{
		index:   map[mappedFileKey]uint64{},
		entries: map[uint64]map[string]interface{}{},
		elfBack: map[uint64][]uint64{},
		next:    1,
	}
}

// resolve returns this mapping's mapped_files id, creating a new catalog
// entry the first time a given device+inode is seen and reusing it for
// every later mapping of the same file. Only file-backed mappings get an
// entry (a bare "false, false" for anonymous/special mappings) -- reported
// via the bool so the caller can omit the "file" reference entirely.
func (c *mappedFilesCatalog) resolve(mm *wire.MemoryMap) (uint64, bool) {
	if mm.MappingType != wire.MappingFile {
		return 0, false
	}
	haveMetadata := mm.FileMetadataStatus == wire.DataAvailable
	var key mappedFileKey
	if haveMetadata {
		key = mappedFileKey{mm.FileDeviceMajor, mm.FileDeviceMinor, mm.FileInode}
		if id, ok := c.index[key]; ok {
			return id, true
		}
	}

	id := c.next
	c.next++
	if haveMetadata {
		c.index[key] = id
	}

	pathEntry := map[string]interface{}{"collection_status": dataStatusName(mm.PathStatus)}
	if mm.PathStatus == wire.DataAvailable {
		pathEntry["value"] = mm.Path
		pathEntry["error"] = nil
	} else {
		pathEntry["value"] = nil
		pathEntry["error"] = errorOrNil(mm.PathError)
	}

	identity := map[string]interface{}{"collection_status": dataStatusName(mm.FileMetadataStatus)}
	if haveMetadata {
		identity["error"] = nil
		identity["device"] = map[string]interface{}{"major": mm.FileDeviceMajor, "minor": mm.FileDeviceMinor}
		identity["inode"] = strU64(mm.FileInode)
		identity["mode"] = mm.FileMode
		identity["size"] = strU64(mm.FileSize)
		identity["mtime_ns"] = isoNanos(mm.FileMtimeNS)
		identity["ctime_ns"] = isoNanos(mm.FileCtimeNS)
		identity["filesystem_magic"] = hexPad64(mm.FileFilesystemMagic)
	} else {
		identity["error"] = errorOrNil(mm.FileMetadataError)
		identity["device"] = nil
		identity["inode"] = nil
		identity["mode"] = nil
		identity["size"] = nil
		identity["mtime_ns"] = nil
		identity["ctime_ns"] = nil
		identity["filesystem_magic"] = nil
	}

	entry := map[string]interface{}{
		"deleted":  mm.FileFlags&0x1 != 0,
		"path":     pathEntry,
		"identity": identity,
	}
	if mm.ELFObjectID != 0 {
		entry["elf"] = mm.ELFObjectID
		c.elfBack[mm.ELFObjectID] = append(c.elfBack[mm.ELFObjectID], id)
	} else {
		entry["elf"] = nil
	}
	c.entries[id] = entry
	return id, true
}

func (c *mappedFilesCatalog) render() map[string]interface{} {
	out := make(map[string]interface{}, len(c.entries))
	for id, entry := range c.entries {
		out[idKey(id)] = entry
	}
	return out
}

func mappingPermissionsString(vmFlags uint64) string {
	b := []byte{'-', '-', '-'}
	if vmFlags&vmFlagRead != 0 {
		b[0] = 'R'
	}
	if vmFlags&vmFlagWrite != 0 {
		b[1] = 'W'
	}
	if vmFlags&vmFlagExec != 0 {
		b[2] = 'X'
	}
	return string(b)
}

func mappingPermissionLimitsString(vmFlags uint64) string {
	b := []byte{'-', '-', '-'}
	if vmFlags&vmFlagMayRead != 0 {
		b[0] = 'R'
	}
	if vmFlags&vmFlagMayWrite != 0 {
		b[1] = 'W'
	}
	if vmFlags&vmFlagMayExec != 0 {
		b[2] = 'X'
	}
	return string(b)
}

func viewMemoryMap(mm *wire.MemoryMap, catalog *mappedFilesCatalog) map[string]interface{} {
	sharing := "PRIVATE"
	if mm.VMFlags&vmFlagShared != 0 {
		sharing = "SHARED"
	}
	m := map[string]interface{}{
		"type":              mappingTypeName(mm.MappingType),
		"start":             hexRaw(mm.StartAddress),
		"end":               hexRaw(mm.EndAddress),
		"permissions":       mappingPermissionsString(mm.VMFlags),
		"permission_limits": mappingPermissionLimitsString(mm.VMFlags),
		"sharing":           sharing,
		"may_share":         mm.VMFlags&vmFlagMayShare != 0,
		"vm_flags":          flagNames(mm.VMFlags&^0xff, vmFlagNameBits),
		"offset_bytes":      strU64(mm.PageOffset),
	}
	if id, ok := catalog.resolve(mm); ok {
		m["file"] = id
	}
	return m
}

func viewMemoryMapsObject(o *moduleclient.MemoryMapsObject, catalog *mappedFilesCatalog) map[string]interface{} {
	mappings := make([]map[string]interface{}, 0, len(o.Mappings))
	for _, mm := range o.Mappings {
		mappings = append(mappings, viewMemoryMap(mm, catalog))
	}
	return map[string]interface{}{
		"collection_status": objectStatusName(o.Status),
		"page_size_bytes":   o.Metadata.PageSizeBytes,
		"mappings":          mappings,
	}
}

func viewELFFile(o *moduleclient.ELFObject, catalog *elfSymbolCatalog, mappedFileIDs []uint64) map[string]interface{} {
	md := &o.Metadata
	header := map[string]interface{}{
		"class":             elfClassName(md.ELFClass),
		"data_encoding":     elfDataEncodingName(md.DataEncoding),
		"os_abi":            md.OSABI,
		"abi_version":       md.ABIVersion,
		"object_type":       elfObjectTypeName(md.ObjectType),
		"object_type_value": md.ObjectType,
		"machine":           elfMachineName(md.Machine),
		"machine_value":     md.Machine,
		"version":           md.ELFVersion,
		"flags":             hexPad32(md.Flags),
		"entry_address":     hexPad64(md.EntryAddress),
		"program_headers": map[string]interface{}{
			"offset": strU64(md.ProgramHeaderOffset), "entry_size": md.ProgramHeaderEntrySize, "count": md.ProgramHeaderCount,
		},
		"section_headers": map[string]interface{}{
			"offset": strU64(md.SectionHeaderOffset), "entry_size": md.SectionHeaderEntrySize, "count": md.SectionHeaderCount, "name_index": md.SectionNameIndex,
		},
	}
	fileIdentity := map[string]interface{}{
		"device":           map[string]interface{}{"major": md.FileDeviceMajor, "minor": md.FileDeviceMinor},
		"inode":            strU64(md.FileInode),
		"mode":             md.FileMode,
		"size":             strU64(md.FileSize),
		"mtime_ns":         isoNanos(md.FileMtimeNS),
		"ctime_ns":         isoNanos(md.FileCtimeNS),
		"filesystem_magic": hexPad64(md.FileFilesystemMagic),
	}

	var interpreter, needed, soname, rpath, runpath, buildIDs []string
	for _, s := range o.Strings {
		val := decodeELFStringValue(s.StringKind, s.Bytes)
		switch s.StringKind {
		case wire.ELFStringInterpreter:
			interpreter = append(interpreter, val)
		case wire.ELFStringNeeded:
			needed = append(needed, val)
		case wire.ELFStringSoname:
			soname = append(soname, val)
		case wire.ELFStringRpath:
			rpath = append(rpath, val)
		case wire.ELFStringRunpath:
			runpath = append(runpath, val)
		case wire.ELFStringBuildID:
			buildIDs = append(buildIDs, val)
		}
	}
	strOrEmpty := func(s []string) []string {
		if s == nil {
			return []string{}
		}
		return s
	}
	if mappedFileIDs == nil {
		mappedFileIDs = []uint64{}
	}

	return map[string]interface{}{
		"collection_status": objectStatusName(o.Status),
		"error":             nil,
		"header":            header,
		"file_identity":     fileIdentity,
		"mapped_files":      mappedFileIDs,
		"interpreter":       strOrEmpty(interpreter),
		"needed":            strOrEmpty(needed),
		"soname":            strOrEmpty(soname),
		"rpath":             strOrEmpty(rpath),
		"runpath":           strOrEmpty(runpath),
		"build_ids":         strOrEmpty(buildIDs),
		"imports": map[string]interface{}{
			"groups": viewELFSymbolGroups(o.Symbols, catalog, wire.ELFSymbolImport, false),
		},
		"exports": map[string]interface{}{
			"groups": viewELFSymbolGroups(o.Symbols, catalog, wire.ELFSymbolExport, true),
		},
	}
}

// elfSymbolCatalog interns ELF symbol names into a single snapshot-wide
// table, referenced by index instead of repeated inline. Symbol names repeat
// enormously across shared libraries (every consumer of libc imports
// "malloc", "printf", ...), so on a system with many mapped shared objects
// this dwarfs every other size lever in the schema.
type elfSymbolCatalog struct {
	index map[string]int
	names []string
}

func newELFSymbolCatalog() *elfSymbolCatalog {
	return &elfSymbolCatalog{index: make(map[string]int)}
}

func (c *elfSymbolCatalog) intern(name string) int {
	if i, ok := c.index[name]; ok {
		return i
	}
	i := len(c.names)
	c.index[name] = i
	c.names = append(c.names, name)
	return i
}

// elfSymbolGroupKey groups symbols the same way ELF's own symbol table
// naturally clusters them -- by (type, binding, visibility), plus
// section_index for exports (a defined symbol's section is meaningful; an
// import/undefined symbol's isn't, so imports don't key or carry it) -- so a
// symbol's per-group metadata is paid for once per group, not once per
// symbol. Matches v8's own imports/exports grouping exactly.
type elfSymbolGroupKey struct {
	symbolType   string
	binding      string
	visibility   string
	sectionIndex uint16
}

func viewELFSymbolGroups(symbols []*wire.ELFSymbol, catalog *elfSymbolCatalog, kind wire.ELFSymbolKind, includeSection bool) []map[string]interface{} {
	var order []elfSymbolGroupKey
	groups := map[elfSymbolGroupKey][]map[string]interface{}{}
	for _, s := range symbols {
		if s.SymbolKind != kind {
			continue
		}
		key := elfSymbolGroupKey{
			symbolType: elfSymbolTypeName(s.SymbolType),
			binding:    elfBindingName(s.Binding),
			visibility: elfVisibilityName(s.Visibility),
		}
		if includeSection {
			key.sectionIndex = s.SectionIndex
		}
		entry := map[string]interface{}{"name": catalog.intern(s.Name)}
		if includeSection {
			entry["value"] = hexRaw(s.Value)
			entry["size_bytes"] = strU64(s.Size)
		}
		if _, ok := groups[key]; !ok {
			order = append(order, key)
		}
		groups[key] = append(groups[key], entry)
	}
	result := make([]map[string]interface{}, 0, len(order))
	for _, key := range order {
		g := map[string]interface{}{
			"type":       key.symbolType,
			"binding":    key.binding,
			"visibility": key.visibility,
			"symbols":    groups[key],
		}
		if includeSection {
			g["section_index"] = key.sectionIndex
		}
		result = append(result, g)
	}
	return result
}

// --- tasks: processes/kthreads/unmatched_threads, matching v8's own split
// (a process's own threads nest under it; a thread whose parent tgid isn't
// in this snapshot -- a race -- goes to unmatched_threads instead).

func viewTaskGroups(t *moduleclient.ProcessTask) map[string]interface{} {
	ref := func(id *uint64) interface{} {
		if id == nil {
			return nil
		}
		return *id
	}
	return map[string]interface{}{
		"identity":    ref(t.IdentityID),
		"environment": ref(t.EnvironmentID),
		"security":    ref(t.SecurityID),
		"context":     ref(t.ContextID),
		"resources":   ref(t.ResourcesID),
		"fds":         ref(t.FilesID),
		"network":     ref(t.NetworkID),
		"memory_maps": ref(t.MemoryMapsID),
	}
}

func viewTaskCore(t *moduleclient.ProcessTask) map[string]interface{} {
	return map[string]interface{}{
		"pid":               t.PID,
		"tgid":              t.TGID,
		"ppid":              t.PPID,
		"start_boottime_ns": strU64(t.StartBoottimeNS),
		"state":             taskStateName(t.State),
		"comm":              t.Comm,
		"groups":            viewTaskGroups(t),
	}
}

func viewTasksSection(snap *moduleclient.ProcessSnapshot) map[string]interface{} {
	processesByTGID := map[uint32]*moduleclient.ProcessTask{}
	for _, t := range snap.Tasks {
		if t.Type == wire.TaskProcess {
			processesByTGID[t.TGID] = t
		}
	}

	var processOrder []*moduleclient.ProcessTask
	var kthreads []map[string]interface{}
	var unmatchedThreads []map[string]interface{}
	threadsByTGID := map[uint32][]map[string]interface{}{}

	for _, t := range snap.Tasks {
		switch t.Type {
		case wire.TaskProcess:
			processOrder = append(processOrder, t)
		case wire.TaskKThread:
			kthreads = append(kthreads, viewTaskCore(t))
		case wire.TaskThread:
			if _, ok := processesByTGID[t.TGID]; ok {
				threadsByTGID[t.TGID] = append(threadsByTGID[t.TGID], viewTaskCore(t))
			} else {
				unmatchedThreads = append(unmatchedThreads, viewTaskCore(t))
			}
		}
	}

	processes := make([]map[string]interface{}, 0, len(processOrder))
	for _, p := range processOrder {
		threads := threadsByTGID[p.TGID]
		if threads == nil {
			threads = []map[string]interface{}{}
		}
		processes = append(processes, map[string]interface{}{
			"process": viewTaskCore(p),
			"threads": threads,
		})
	}
	if kthreads == nil {
		kthreads = []map[string]interface{}{}
	}
	if unmatchedThreads == nil {
		unmatchedThreads = []map[string]interface{}{}
	}

	return map[string]interface{}{
		"processes":         processes,
		"unmatched_threads": unmatchedThreads,
		"kthreads":          kthreads,
	}
}

// --- top-level snapshot assembly ---

// apiProcessSnapshot builds the API response mirroring pslist-v8.json's own
// top-level shape: schema_version + snapshot (envelope) + tasks + objects.
func apiProcessSnapshot(snap *moduleclient.ProcessSnapshot) (api.ProcessSnapshot, error) {
	identities := map[string]interface{}{}
	for id, o := range snap.Identities {
		identities[idKey(id)] = viewIdentity(o)
	}
	environments := map[string]interface{}{}
	for id, o := range snap.Environments {
		environments[idKey(id)] = viewEnvironment(o)
	}
	securities := map[string]interface{}{}
	for id, o := range snap.Securities {
		securities[idKey(id)] = viewSecurity(o)
	}
	contexts := map[string]interface{}{}
	for id, o := range snap.Contexts {
		contexts[idKey(id)] = viewContext(o)
	}
	resources := map[string]interface{}{}
	for id, o := range snap.Resources {
		resources[idKey(id)] = viewResources(o)
	}
	fds := map[string]interface{}{}
	network := map[string]interface{}{}
	for id, o := range snap.FilesObjects {
		fds[idKey(id)] = viewFilesObject(o)
		network[idKey(id)] = viewNetworkObject(o)
	}

	// memory_maps must run before elf: the mapped_files catalog it builds
	// (deduplicating file identity across every task's mappings) is what
	// elf's own "mapped_files" back-reference array reads from.
	filesCatalog := newMappedFilesCatalog()
	memoryMaps := map[string]interface{}{}
	for id, o := range snap.MemoryMaps {
		memoryMaps[idKey(id)] = viewMemoryMapsObject(o, filesCatalog)
	}

	symbolCatalog := newELFSymbolCatalog()
	elfFiles := map[string]interface{}{}
	for id, o := range snap.ELFs {
		elfFiles[idKey(id)] = viewELFFile(o, symbolCatalog, filesCatalog.elfBack[id])
	}
	elfSymbolNames := symbolCatalog.names
	if elfSymbolNames == nil {
		elfSymbolNames = []string{}
	}

	objects := map[string]interface{}{
		"identity":     identities,
		"environment":  environments,
		"security":     securities,
		"context":      contexts,
		"resources":    resources,
		"fds":          fds,
		"network":      network,
		"mapped_files": filesCatalog.render(),
		"memory_maps":  memoryMaps,
		"elf": map[string]interface{}{
			"symbol_names": elfSymbolNames,
			"files":        elfFiles,
		},
	}

	snapshotStatus := "complete"
	if snap.Status == wire.SnapshotPartial {
		snapshotStatus = "partial"
	}
	snapshotEnvelope := map[string]interface{}{
		"status":            snapshotStatus,
		"start_boottime_ns": strU64(snap.StartBoottimeNS),
		"end_boottime_ns":   strU64(snap.EndBoottimeNS),
		"requested_groups":  groupsToAPI(snap.RequestedGroups),
		"effective_groups":  groupsToAPI(snap.EffectiveGroups),
		// Extras beyond v8's own snapshot shape: real info the wire
		// protocol reports that v8 just doesn't surface, kept rather than
		// discarded.
		"supported_groups": groupsToAPI(snap.SupportedGroups),
		"task_count":       snap.TaskCount,
		"object_count":     snap.ObjectCount,
	}

	return api.ProcessSnapshot{
		SchemaVersion: 1,
		Snapshot:      snapshotEnvelope,
		Tasks:         viewTasksSection(snap),
		Objects:       objects,
	}, nil
}
