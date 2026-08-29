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

// This file is the server-side "final data" layer for pslist: it turns the
// internal wire/moduleclient Go types into the JSON the API actually
// returns. Every enum, bitmask, timestamp, address and permission field is
// parsed to a real value here -- the TUI (or any other frontend) only
// arranges and displays what's already final, it never re-parses a raw
// wire code itself. Each view function below is explicit and field-by-field
// on purpose, not a generic reflection-based renamer, so every choice is
// easy to find and re-review.

// --- small shared formatters ---

func hexU64(v uint64) string { return fmt.Sprintf("0x%x", v) }

// isoNanos formats an epoch-nanosecond timestamp (Identity/MemoryMap/ELF
// mtime/ctime, all real points in time) as UTC ISO-8601.
func isoNanos(ns int64) string {
	return time.Unix(0, ns).UTC().Format(time.RFC3339)
}

// isoSecNsec combines a struct-timespec-style seconds+nanoseconds pair
// (FDStat's four *_time fields) into one ISO-8601 string.
func isoSecNsec(sec int64, nsec uint32) string {
	return time.Unix(sec, int64(nsec)).UTC().Format(time.RFC3339)
}

// unixMode renders a POSIX stat mode as its "ls -l" form: a file-type
// character followed by the rwxrwxrwx permission triplet. The octal value
// is intentionally not repeated alongside it -- the letters determine it
// exactly, so showing both would be the raw+decoded duplication this
// design avoids elsewhere (see the mapping permissions field).
func unixMode(mode uint32) string {
	const (
		sIFMT   = 0o170000
		sIFSOCK = 0o140000
		sIFLNK  = 0o120000
		sIFREG  = 0o100000
		sIFBLK  = 0o060000
		sIFDIR  = 0o040000
		sIFCHR  = 0o020000
		sIFIFO  = 0o010000
	)
	typeChar := byte('?')
	switch mode & sIFMT {
	case sIFSOCK:
		typeChar = 's'
	case sIFLNK:
		typeChar = 'l'
	case sIFREG:
		typeChar = '-'
	case sIFBLK:
		typeChar = 'b'
	case sIFDIR:
		typeChar = 'd'
	case sIFCHR:
		typeChar = 'c'
	case sIFIFO:
		typeChar = 'p'
	}
	perm := func(bit uint32, c byte) byte {
		if mode&bit != 0 {
			return c
		}
		return '-'
	}
	bits := []byte{
		typeChar,
		perm(0o400, 'r'), perm(0o200, 'w'), perm(0o100, 'x'),
		perm(0o040, 'r'), perm(0o020, 'w'), perm(0o010, 'x'),
		perm(0o004, 'r'), perm(0o002, 'w'), perm(0o001, 'x'),
	}
	if mode&0o4000 != 0 {
		if bits[3] == 'x' {
			bits[3] = 's'
		} else {
			bits[3] = 'S'
		}
	}
	if mode&0o2000 != 0 {
		if bits[6] == 'x' {
			bits[6] = 's'
		} else {
			bits[6] = 'S'
		}
	}
	if mode&0o1000 != 0 {
		if bits[9] == 'x' {
			bits[9] = 't'
		} else {
			bits[9] = 'T'
		}
	}
	return string(bits)
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
		names = append(names, fmt.Sprintf("unknown_%s", hexU64(remaining)))
	}
	sort.Strings(names)
	return names
}

// errnoName maps the stable, POSIX-standard low-numbered errno codes (plus
// the common socket-layer ones) to their symbolic name. Anything else
// renders as "errno_N" rather than guessing.
func errnoName(code uint32) string {
	if code == 0 {
		return ""
	}
	if name, ok := errnoNames[code]; ok {
		return name
	}
	return fmt.Sprintf("errno_%d", code)
}

var errnoNames = map[uint32]string{
	1: "EPERM", 2: "ENOENT", 3: "ESRCH", 4: "EINTR", 5: "EIO", 6: "ENXIO",
	7: "E2BIG", 8: "ENOEXEC", 9: "EBADF", 10: "ECHILD", 11: "EAGAIN",
	12: "ENOMEM", 13: "EACCES", 14: "EFAULT", 15: "ENOTBLK", 16: "EBUSY",
	17: "EEXIST", 18: "EXDEV", 19: "ENODEV", 20: "ENOTDIR", 21: "EISDIR",
	22: "EINVAL", 23: "ENFILE", 24: "EMFILE", 25: "ENOTTY", 26: "ETXTBSY",
	27: "EFBIG", 28: "ENOSPC", 29: "ESPIPE", 30: "EROFS", 31: "EMLINK",
	32: "EPIPE", 33: "EDOM", 34: "ERANGE", 35: "EDEADLK", 36: "ENAMETOOLONG",
	37: "ENOLCK", 38: "ENOSYS", 39: "ENOTEMPTY", 40: "ELOOP",
	98: "EADDRINUSE", 99: "EADDRNOTAVAIL", 101: "ENETUNREACH",
	104: "ECONNRESET", 106: "EISCONN", 107: "ENOTCONN", 110: "ETIMEDOUT",
	111: "ECONNREFUSED", 112: "EHOSTDOWN", 113: "EHOSTUNREACH",
}

// fsMagicName maps the common Linux filesystem superblock magic numbers
// (linux/magic.h) to their filesystem name. Falls back to hex for anything
// not in this small well-known set.
func fsMagicName(magic uint64) string {
	if name, ok := fsMagicNames[magic]; ok {
		return name
	}
	return hexU64(magic)
}

var fsMagicNames = map[uint64]string{
	0xEF53:     "ext2/3/4",
	0x01021994: "tmpfs",
	0x9fa0:     "proc",
	0x62656572: "sysfs",
	0x1cd1:     "devpts",
	0x63677270: "cgroup2",
	0x27e0eb:   "cgroup",
	0x9123683E: "btrfs",
	0x58465342: "xfs",
	0x794c7630: "overlayfs",
	0x6969:     "nfs",
	0x73717368: "squashfs",
	0x858458f6: "ramfs/hugetlbfs",
	0x42494e4d: "binfmt_misc",
	0xf97cff8c: "selinuxfs",
	0x50495045: "pipefs",
	0x534F434B: "sockfs",
	0x09041934: "anon_inodefs",
	0x19800202: "mqueue",
	0x64626720: "debugfs",
	0x74726163: "tracefs",
	0xcafe4a11: "bpf",
}

// --- enum name tables ---

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

func taskTypeName(t wire.TaskType) string {
	switch t {
	case wire.TaskProcess:
		return "process"
	case wire.TaskKThread:
		return "kthread"
	case wire.TaskThread:
		return "thread"
	default:
		return fmt.Sprintf("unknown_%d", t)
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
		return "SCHED_NORMAL"
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

func mappingNameKindName(k wire.MappingNameKind) string {
	switch k {
	case wire.MappingNameNone:
		return "none"
	case wire.MappingNameHeap:
		return "heap"
	case wire.MappingNameStack:
		return "stack"
	case wire.MappingNameAnonymous:
		return "anonymous"
	case wire.MappingNameSharedAnonymous:
		return "shared_anonymous"
	case wire.MappingNameSpecial:
		return "special"
	default:
		return fmt.Sprintf("unknown_%d", k)
	}
}

func elfStringKindName(k wire.ELFStringKind) string {
	switch k {
	case wire.ELFStringInterpreter:
		return "interpreter"
	case wire.ELFStringNeeded:
		return "needed"
	case wire.ELFStringSoname:
		return "soname"
	case wire.ELFStringRpath:
		return "rpath"
	case wire.ELFStringRunpath:
		return "runpath"
	case wire.ELFStringBuildID:
		return "build_id"
	default:
		return fmt.Sprintf("unknown_%d", k)
	}
}

// elfClassName, elfDataEncodingName, elfOSABIName, elfObjectTypeName,
// elfMachineName, elfBindingName, elfSymbolTypeName and elfVisibilityName
// are standard ELF header constants (elf.h), not part of the mapple wire
// protocol.
func elfClassName(c uint8) string {
	switch c {
	case 1:
		return "ELFCLASS32"
	case 2:
		return "ELFCLASS64"
	default:
		return fmt.Sprintf("unknown_%d", c)
	}
}

func elfDataEncodingName(c uint8) string {
	switch c {
	case 1:
		return "ELFDATA2LSB"
	case 2:
		return "ELFDATA2MSB"
	default:
		return fmt.Sprintf("unknown_%d", c)
	}
}

func elfOSABIName(c uint8) string {
	switch c {
	case 0:
		return "ELFOSABI_NONE"
	case 1:
		return "ELFOSABI_HPUX"
	case 2:
		return "ELFOSABI_NETBSD"
	case 3:
		return "ELFOSABI_LINUX"
	case 6:
		return "ELFOSABI_SOLARIS"
	case 9:
		return "ELFOSABI_FREEBSD"
	case 97:
		return "ELFOSABI_ARM"
	case 255:
		return "ELFOSABI_STANDALONE"
	default:
		return fmt.Sprintf("unknown_%d", c)
	}
}

func elfObjectTypeName(t uint16) string {
	switch t {
	case 0:
		return "ET_NONE"
	case 1:
		return "ET_REL"
	case 2:
		return "ET_EXEC"
	case 3:
		return "ET_DYN"
	case 4:
		return "ET_CORE"
	default:
		return fmt.Sprintf("unknown_%d", t)
	}
}

func elfMachineName(m uint16) string {
	switch m {
	case 3:
		return "EM_386"
	case 8:
		return "EM_MIPS"
	case 40:
		return "EM_ARM"
	case 62:
		return "EM_X86_64"
	case 183:
		return "EM_AARCH64"
	case 243:
		return "EM_RISCV"
	default:
		return fmt.Sprintf("unknown_%d", m)
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
		return "regular"
	case wire.FDDirectory:
		return "directory"
	case wire.FDCharacter:
		return "character"
	case wire.FDBlock:
		return "block"
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

// addressFamilyName, socketTypeName, protocolName, socketStateName and
// protocolStateName are standard Linux/POSIX values (include/linux/socket.h,
// net/tcp_states.h), not part of the mapple wire protocol. protocolName/
// protocolStateName are only meaningful for an actual INET/INET6 socket --
// the kernel reuses the same numeric range for other families to mean
// something else entirely (a netlink protocol number, an unused state), so
// those fall back to a plain "raw_N" string instead of a wrong IPPROTO_*/
// TCP_* name.
func addressFamilyName(f uint16) string {
	switch f {
	case 0:
		return "AF_UNSPEC"
	case 1:
		return "AF_UNIX"
	case 2:
		return "AF_INET"
	case 10:
		return "AF_INET6"
	case 16:
		return "AF_NETLINK"
	case 17:
		return "AF_PACKET"
	case 38:
		return "AF_ALG"
	case 40:
		return "AF_VSOCK"
	case 42:
		return "AF_QIPCRTR"
	default:
		return fmt.Sprintf("AF_%d", f)
	}
}

func socketTypeName(t uint16) string {
	switch t {
	case 1:
		return "SOCK_STREAM"
	case 2:
		return "SOCK_DGRAM"
	case 3:
		return "SOCK_RAW"
	case 4:
		return "SOCK_RDM"
	case 5:
		return "SOCK_SEQPACKET"
	case 6:
		return "SOCK_DCCP"
	case 10:
		return "SOCK_PACKET"
	default:
		return fmt.Sprintf("SOCK_%d", t)
	}
}

func isInetFamily(family uint16) bool { return family == 2 || family == 10 }

func protocolName(family, p uint16) string {
	if !isInetFamily(family) {
		return fmt.Sprintf("raw_%d", p)
	}
	switch p {
	case 0:
		return "IPPROTO_IP"
	case 1:
		return "IPPROTO_ICMP"
	case 2:
		return "IPPROTO_IGMP"
	case 6:
		return "IPPROTO_TCP"
	case 17:
		return "IPPROTO_UDP"
	case 41:
		return "IPPROTO_IPV6"
	case 58:
		return "IPPROTO_ICMPV6"
	case 132:
		return "IPPROTO_SCTP"
	default:
		return fmt.Sprintf("IPPROTO_%d", p)
	}
}

func socketStateName(s uint16) string {
	switch s {
	case 0:
		return "SS_FREE"
	case 1:
		return "SS_UNCONNECTED"
	case 2:
		return "SS_CONNECTING"
	case 3:
		return "SS_CONNECTED"
	case 4:
		return "SS_DISCONNECTING"
	default:
		return fmt.Sprintf("SS_%d", s)
	}
}

func protocolStateName(family, s uint16) string {
	if !isInetFamily(family) {
		return fmt.Sprintf("raw_%d", s)
	}
	switch s {
	case 1:
		return "TCP_ESTABLISHED"
	case 2:
		return "TCP_SYN_SENT"
	case 3:
		return "TCP_SYN_RECV"
	case 4:
		return "TCP_FIN_WAIT1"
	case 5:
		return "TCP_FIN_WAIT2"
	case 6:
		return "TCP_TIME_WAIT"
	case 7:
		return "TCP_CLOSE"
	case 8:
		return "TCP_CLOSE_WAIT"
	case 9:
		return "TCP_LAST_ACK"
	case 10:
		return "TCP_LISTEN"
	case 11:
		return "TCP_CLOSING"
	case 12:
		return "TCP_NEW_SYN_RECV"
	default:
		return fmt.Sprintf("TCP_%d", s)
	}
}

// --- bitmask -> name-array tables ---

var executableFlagBits = []flagBit{
	{uint64(wire.ExecutableFlagDeleted), "deleted"},
	{uint64(wire.ExecutableFlagPathUnavailable), "path_unavailable"},
	{uint64(wire.ExecutableFlagMetadataUnavailable), "metadata_unavailable"},
}

var namespaceBits = []flagBit{
	{1 << 0, "user"}, {1 << 1, "uts"}, {1 << 2, "ipc"}, {1 << 3, "mount"},
	{1 << 4, "pid_active"}, {1 << 5, "pid_children"}, {1 << 6, "network"},
	{1 << 7, "cgroup"}, {1 << 8, "time"}, {1 << 9, "time_children"},
}

var secbits = []flagBit{
	{1 << 0, "NOROOT"}, {1 << 1, "NOROOT_LOCKED"},
	{1 << 2, "NO_SETUID_FIXUP"}, {1 << 3, "NO_SETUID_FIXUP_LOCKED"},
	{1 << 4, "KEEP_CAPS"}, {1 << 5, "KEEP_CAPS_LOCKED"},
	{1 << 6, "NO_CAP_AMBIENT_RAISE"}, {1 << 7, "NO_CAP_AMBIENT_RAISE_LOCKED"},
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

var memoryMapFileFlagBits = []flagBit{{1 << 0, "deleted"}}

// vmFlagBits is Linux's VM_* mapping flags, minus the 4 fundamental ones
// (READ/WRITE/EXEC/SHARED) which get their own "permissions"/"shared"
// fields on a memory_map instead -- see viewMemoryMap.
var vmFlagBits = []flagBit{
	{1 << 4, "MAYREAD"}, {1 << 5, "MAYWRITE"}, {1 << 6, "MAYEXEC"}, {1 << 7, "MAYSHARE"},
	{1 << 8, "GROWSDOWN"}, {1 << 9, "UFFD_MISSING"}, {1 << 10, "PFNMAP"},
	{1 << 12, "UFFD_WP"}, {1 << 13, "LOCKED"}, {1 << 14, "IO"},
	{1 << 15, "SEQ_READ"}, {1 << 16, "RAND_READ"}, {1 << 17, "DONTCOPY"},
	{1 << 18, "DONTEXPAND"}, {1 << 19, "LOCKONFAULT"}, {1 << 20, "ACCOUNT"},
	{1 << 21, "NORESERVE"}, {1 << 22, "HUGETLB"}, {1 << 23, "SYNC"},
	{1 << 25, "WIPEONFORK"}, {1 << 26, "DONTDUMP"}, {1 << 27, "SOFTDIRTY"},
	{1 << 28, "MIXEDMAP"}, {1 << 29, "HUGEPAGE"}, {1 << 30, "NOHUGEPAGE"},
	{1 << 31, "MERGEABLE"},
}

const (
	vmFlagRead   = 1 << 0
	vmFlagWrite  = 1 << 1
	vmFlagExec   = 1 << 2
	vmFlagShared = 1 << 3
)

// statxBits mirrors the stable UAPI STATX_* bits (linux/stat.h) describing
// which stat fields the kernel actually populated.
var statxBits = []flagBit{
	{0x001, "TYPE"}, {0x002, "MODE"}, {0x004, "NLINK"}, {0x008, "UID"},
	{0x010, "GID"}, {0x020, "ATIME"}, {0x040, "MTIME"}, {0x080, "CTIME"},
	{0x100, "INO"}, {0x200, "SIZE"}, {0x400, "BLOCKS"}, {0x800, "BTIME"},
	{0x1000, "MNT_ID"}, {0x2000, "DIOALIGN"},
}

// fmodeBits covers only the small subset of the kernel-internal f_mode
// bitmask (linux/fs.h) that has been stable across kernel versions; f_mode
// is not stable UAPI, so anything outside this subset renders as
// "unknown_0x..." rather than risk a wrong name for a bit whose meaning can
// differ by kernel version.
var fmodeBits = []flagBit{
	{0x1, "READ"}, {0x2, "WRITE"}, {0x4, "LSEEK"}, {0x8, "PREAD"},
	{0x10, "PWRITE"}, {0x20, "EXEC"}, {0x40, "NDELAY"}, {0x80, "EXCL"},
}

// fsAttrBits mirrors the stable UAPI FS_*_FL inode attribute bits
// (linux/fs.h) reported via statx/FS_IOC_GETFLAGS.
var fsAttrBits = []flagBit{
	{0x1, "SECRM"}, {0x2, "UNRM"}, {0x4, "COMPR"}, {0x8, "SYNC"},
	{0x10, "IMMUTABLE"}, {0x20, "APPEND"}, {0x40, "NODUMP"}, {0x80, "NOATIME"},
	{0x100, "DIRTY"}, {0x200, "COMPRBLK"}, {0x400, "NOCOMP"}, {0x800, "ENCRYPT"},
	{0x1000, "BTREE"}, {0x2000, "INDEX"}, {0x8000, "TOPDIR"}, {0x10000, "HUGE_FILE"},
	{0x20000, "EXTENT"}, {0x100000, "EA_INODE"}, {0x200000, "EOFBLOCKS"},
	{0x400000, "NOCOW"}, {0x2000000, "INLINE_DATA"}, {0x4000000, "PROJINHERIT"},
	{0x10000000, "CASEFOLD"}, {0x80000000, "RESERVED"},
}

var openFlagBits = []flagBit{
	{0o100, "CREAT"}, {0o200, "EXCL"}, {0o400, "NOCTTY"}, {0o1000, "TRUNC"},
	{0o2000, "APPEND"}, {0o4000, "NONBLOCK"}, {0o10000, "DSYNC"}, {0o20000, "ASYNC"},
	{0o40000, "DIRECT"}, {0o100000, "LARGEFILE"}, {0o200000, "DIRECTORY"},
	{0o400000, "NOFOLLOW"}, {0o1000000, "NOATIME"}, {0o2000000, "CLOEXEC"},
	{0o10000000, "PATH"}, {0o20000000, "TMPFILE"},
}

// openAccessMode returns the access-mode name from open(2)'s low 2 bits,
// which aren't independent flags (O_RDONLY is literally 0), plus the
// remaining bits' names via openFlagBits.
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

func idKey(id uint64) string { return strconv.FormatUint(id, 10) }

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

// --- per-group view builders ---
//
// Each function below builds the final JSON object for one wire object,
// explicitly field by field. Status-gated sub-fields (an executable's own
// metadata, a namespace's own inode, a socket endpoint's netlink fields)
// are only set when the data is actually applicable, per the omission
// rule -- everything else (bitmask arrays, booleans, enum strings) is
// always present so the shape is predictable.

func viewTask(t *moduleclient.ProcessTask) map[string]interface{} {
	m := map[string]interface{}{
		"pid":               t.PID,
		"tgid":              t.TGID,
		"ppid":              t.PPID,
		"start_boottime_ns": t.StartBoottimeNS,
		"state":             taskStateName(t.State),
		"comm":              t.Comm,
		"type":              taskTypeName(t.Type),
	}
	setID := func(key string, id *uint64) {
		if id != nil {
			m[key] = *id
		}
	}
	setID("identity_id", t.IdentityID)
	setID("security_id", t.SecurityID)
	setID("context_id", t.ContextID)
	setID("resources_id", t.ResourcesID)
	setID("environment_id", t.EnvironmentID)
	setID("memory_maps_id", t.MemoryMapsID)
	setID("files_id", t.FilesID)
	setID("network_id", t.NetworkID)
	return m
}

func viewIdentity(o *moduleclient.IdentityObject) map[string]interface{} {
	m := map[string]interface{}{
		"status":              objectStatusName(o.Status),
		"command_line_status": commandLineStatusName(o.CommandLineStatus),
		"executable_status":   executableStatusName(o.ExecutableStatus),
	}
	if o.CommandLineStatus == wire.CommandLineAvailable || o.CommandLineStatus == wire.CommandLineEmpty {
		m["command_line"] = splitArgv(o.CommandLine)
	}
	for _, f := range flagNames(uint64(o.ExecutableFlags), executableFlagBits) {
		m[f] = true
	}
	if o.ExecutableStatus == wire.ExecutableAvailable {
		if o.ExecutablePath != "" {
			m["executable_path"] = o.ExecutablePath
		}
		m["executable_device_major"] = o.ExecutableDeviceMajor
		m["executable_device_minor"] = o.ExecutableDeviceMinor
		m["executable_mode"] = unixMode(o.ExecutableMode)
		m["executable_inode"] = o.ExecutableInode
		m["executable_size"] = o.ExecutableSize
		m["executable_mtime"] = isoNanos(o.ExecutableMtimeNS)
		m["executable_ctime"] = isoNanos(o.ExecutableCtimeNS)
		m["executable_filesystem"] = fsMagicName(o.ExecutableFilesystemMagic)
	}
	return m
}

func viewSecurity(o *moduleclient.SecurityObject) map[string]interface{} {
	m := map[string]interface{}{
		"status":                   objectStatusName(o.Status),
		"uid":                      o.UID,
		"gid":                      o.GID,
		"effective_uid":            o.EffectiveUID,
		"effective_gid":            o.EffectiveGID,
		"saved_uid":                o.SavedUID,
		"saved_gid":                o.SavedGID,
		"filesystem_uid":           o.FilesystemUID,
		"filesystem_gid":           o.FilesystemGID,
		"user_namespace":           o.UserNamespace,
		"securebits":               flagNames(uint64(o.Securebits), secbits),
		"no_new_privs":             o.NoNewPrivs != 0,
		"seccomp_mode":             seccompModeName(o.SeccompMode),
		"seccomp_filter_count":     o.SeccompFilterCount,
		"supplementary_groups":     o.SupplementaryGroups,
		"dumpable_status":          dataStatusName(o.DumpableStatus),
		"lsm_status":               dataStatusName(o.LSMStatus),
		"lsm_provider_status":      dataStatusName(o.LSMProviderStatus),
		"capabilities_inheritable": flagNames(o.CapabilityInheritable, capabilityBits),
		"capabilities_permitted":   flagNames(o.CapabilityPermitted, capabilityBits),
		"capabilities_effective":   flagNames(o.CapabilityEffective, capabilityBits),
		"capabilities_bounding":    flagNames(o.CapabilityBounding, capabilityBits),
		"capabilities_ambient":     flagNames(o.CapabilityAmbient, capabilityBits),
	}
	if o.DumpableStatus == wire.DataAvailable {
		m["dumpable"] = dumpableName(o.Dumpable)
	} else if o.DumpableReason != 0 {
		m["dumpable_reason"] = dumpableReasonName(o.DumpableReason)
	}
	if o.LSMStatus == wire.DataAvailable {
		m["lsm_secid"] = o.LSMSecID
	}
	if o.LSMError != 0 {
		m["lsm_error"] = errnoName(o.LSMError)
	}
	if o.LSMProviderStatus == wire.DataAvailable {
		m["lsm_provider"] = o.LSMProvider
		m["lsm_context"] = string(o.LSMContext)
	}
	if o.LSMProviderError != 0 {
		m["lsm_provider_error"] = errnoName(o.LSMProviderError)
	}
	return m
}

func viewContext(o *moduleclient.ContextObject) map[string]interface{} {
	available := flagNames(uint64(o.NamespaceAvailableMask), namespaceBits)
	avail := func(name string) bool {
		for _, a := range available {
			if a == name {
				return true
			}
		}
		return false
	}
	m := map[string]interface{}{
		"status":                 objectStatusName(o.Status),
		"namespaces_available":   available,
		"namespaces_unsupported": flagNames(uint64(o.NamespaceUnsupportedMask), namespaceBits),
	}
	if o.CWDStatus == wire.DataAvailable {
		m["cwd"] = o.CWD
	}
	if o.RootStatus == wire.DataAvailable {
		m["root"] = o.Root
	}
	if o.CgroupStatus == wire.DataAvailable {
		m["cgroup_hierarchy_id"] = o.CgroupHierarchyID
		m["cgroup_id"] = o.CgroupID
		m["cgroup_inode"] = o.CgroupInode
	}
	if o.CgroupHostPathStatus == wire.DataAvailable {
		m["cgroup_host_path"] = o.CgroupHostPath
	}
	if o.CgroupNamespacePathStatus == wire.DataAvailable {
		m["cgroup_namespace_path"] = o.CgroupNamespacePath
	}
	if avail("user") {
		m["user_namespace"] = o.UserNamespace
	}
	if avail("uts") {
		m["uts_namespace"] = o.UTSNamespace
	}
	if avail("ipc") {
		m["ipc_namespace"] = o.IPCNamespace
	}
	if avail("pid_active") {
		m["pid_namespace_active"] = o.PIDNamespaceActive
	}
	if avail("pid_children") {
		m["pid_namespace_children"] = o.PIDNamespaceChildren
	}
	if avail("network") {
		m["network_namespace"] = o.NetworkNamespace
	}
	if avail("cgroup") {
		m["cgroup_namespace"] = o.CgroupNamespace
	}
	if avail("time") {
		m["time_namespace"] = o.TimeNamespace
	}
	if avail("time_children") {
		m["time_namespace_children"] = o.TimeNamespaceChildren
	}
	return m
}

func viewResources(o *moduleclient.ResourcesObject) map[string]interface{} {
	m := map[string]interface{}{
		"status":                       objectStatusName(o.Status),
		"memory_status":                dataStatusName(o.MemoryStatus),
		"io_status":                    dataStatusName(o.IOStatus),
		"network_status":               dataStatusName(o.NetworkStatus),
		"current_cpu":                  o.CurrentCPU,
		"scheduling_policy":            schedulingPolicyName(o.SchedulingPolicy),
		"priority":                     o.Priority,
		"nice":                         o.Nice,
		"realtime_priority":            o.RealtimePriority,
		"page_size_bytes":              o.PageSizeBytes,
		"cpu_user_time_sec":            float64(o.CPUUserTimeNS) / 1e9,
		"cpu_system_time_sec":          float64(o.CPUSystemTimeNS) / 1e9,
		"minor_faults":                 o.MinorFaults,
		"major_faults":                 o.MajorFaults,
		"voluntary_context_switches":   o.VoluntaryContextSwitches,
		"involuntary_context_switches": o.InvoluntaryContextSwitches,
		"io_read_characters":           o.IOReadCharacters,
		"io_write_characters":          o.IOWriteCharacters,
		"io_read_syscalls":             o.IOReadSyscalls,
		"io_write_syscalls":            o.IOWriteSyscalls,
		"io_storage_read_bytes":        o.IOStorageReadBytes,
		"io_storage_write_bytes":       o.IOStorageWriteBytes,
		"io_cancelled_write_bytes":     o.IOCancelledWriteBytes,
		"memory_virtual_pages":         o.MemoryVirtualPages,
		"memory_peak_virtual_pages":    o.MemoryPeakVirtualPages,
		"memory_locked_pages":          o.MemoryLockedPages,
		"memory_data_pages":            o.MemoryDataPages,
		"memory_stack_pages":           o.MemoryStackPages,
		"memory_executable_pages":      o.MemoryExecutablePages,
		"memory_resident_pages":        o.MemoryResidentPages,
		"memory_peak_resident_pages":   o.MemoryPeakResidentPages,
		"memory_anonymous_pages":       o.MemoryAnonymousPages,
		"memory_file_pages":            o.MemoryFilePages,
		"memory_shared_pages":          o.MemorySharedPages,
		"memory_swap_entries":          o.MemorySwapEntries,
	}
	if o.MemoryReason != wire.MemoryReasonNone {
		m["memory_reason"] = memoryReasonName(o.MemoryReason)
	}
	if o.MemoryError != 0 {
		m["memory_error"] = errnoName(o.MemoryError)
	}
	if o.IOError != 0 {
		m["io_error"] = errnoName(o.IOError)
	}
	if o.NetworkError != 0 {
		m["network_error"] = errnoName(o.NetworkError)
	}
	return m
}

var environmentFlagBits = []flagBit{{1 << 0, "TERMINATED"}}

func viewEnvironment(o *moduleclient.EnvironmentObject) map[string]interface{} {
	m := map[string]interface{}{
		"status":              objectStatusName(o.Status),
		"availability_status": environmentStatusName(o.AvailabilityStatus),
		"flags":               flagNames(uint64(o.Flags), environmentFlagBits),
	}
	if o.Error != 0 {
		m["error"] = errnoName(o.Error)
	}
	if o.AvailabilityStatus == wire.EnvironmentAvailable {
		env := map[string]string{}
		for _, entry := range o.Entries() {
			key, value, ok := strings.Cut(entry, "=")
			if !ok {
				continue
			}
			env[key] = value
		}
		m["environment"] = env
		m["entry_count"] = o.EntryCount
	}
	return m
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
		"fd":              e.FD.FD,
		"kind":            fdKindName(e.FD.Kind),
		"path_status":     dataStatusName(e.FD.PathStatus),
		"identity_status": dataStatusName(e.FD.IdentityStatus),
		"cloexec":         e.FD.DescriptorFlags&wire.FDDescriptorFlagCloexec != 0,
		"access_mode":     openAccessMode(e.FD.OpenFlags),
		"flags":           flagNames(uint64(e.FD.OpenFlags&^0x3), openFlagBits),
	}
	if e.FD.PathError != 0 {
		m["path_error"] = errnoName(e.FD.PathError)
	}
	if e.FD.IdentityError != 0 {
		m["identity_error"] = errnoName(e.FD.IdentityError)
	}
	if e.FD.PathStatus == wire.DataAvailable && e.FD.Path != "" {
		m["path"] = e.FD.Path
	}
	if e.FD.IdentityStatus == wire.DataAvailable {
		m["device_major"] = e.FD.DeviceMajor
		m["device_minor"] = e.FD.DeviceMinor
		m["inode"] = e.FD.Inode
		m["mount_id"] = e.FD.MountID
	}
	if e.Details != nil {
		m["details"] = viewFDDetails(e.Details)
	}
	if e.Network != nil {
		m["network"] = viewNetwork(e.Network)
	}
	return m
}

func viewFDDetails(d *wire.FDDetails) map[string]interface{} {
	m := map[string]interface{}{
		"stat_status":               dataStatusName(d.StatStatus),
		"fdinfo_status":             dataStatusName(d.FDInfoStatus),
		"file_mode":                 flagNames(d.FileMode, fmodeBits),
		"position":                  d.Position,
		"version":                   d.Version,
		"credential_uid":            d.CredentialUID,
		"credential_gid":            d.CredentialGID,
		"credential_effective_uid":  d.CredentialEffectiveUID,
		"credential_effective_gid":  d.CredentialEffectiveGID,
		"credential_saved_uid":      d.CredentialSavedUID,
		"credential_saved_gid":      d.CredentialSavedGID,
		"credential_filesystem_uid": d.CredentialFilesystemUID,
		"credential_filesystem_gid": d.CredentialFilesystemGID,
	}
	if d.StatError != 0 {
		m["stat_error"] = errnoName(d.StatError)
	}
	if d.FDInfoError != 0 {
		m["fdinfo_error"] = errnoName(d.FDInfoError)
	}
	if d.AsyncOwnerPID != 0 {
		m["async_owner"] = map[string]interface{}{
			"pid":           d.AsyncOwnerPID,
			"type":          d.AsyncOwnerType,
			"signal":        d.AsyncSignal,
			"uid":           d.AsyncOwnerUID,
			"effective_uid": d.AsyncOwnerEffectiveUID,
		}
	}
	if d.StatStatus == wire.DataAvailable {
		m["stat"] = viewFDStat(d.Stat)
	}
	if d.FDInfoStatus == wire.DataAvailable && d.FDInfo != "" {
		m["fdinfo"] = d.FDInfo
	}
	return m
}

func viewFDStat(s wire.FDStat) map[string]interface{} {
	return map[string]interface{}{
		"result_mask":                flagNames(uint64(s.ResultMask), statxBits),
		"mode":                       unixMode(s.Mode),
		"link_count":                 s.LinkCount,
		"block_size":                 s.BlockSize,
		"uid":                        s.UID,
		"gid":                        s.GID,
		"special_device_major":       s.SpecialDeviceMajor,
		"special_device_minor":       s.SpecialDeviceMinor,
		"direct_io_memory_alignment": s.DirectIOMemoryAlignment,
		"direct_io_offset_alignment": s.DirectIOOffsetAlignment,
		"size":                       s.Size,
		"blocks":                     s.Blocks,
		"attributes":                 flagNames(s.Attributes, fsAttrBits),
		"attributes_mask":            flagNames(s.AttributesMask, fsAttrBits),
		"change_cookie":              s.ChangeCookie,
		"filesystem":                 fsMagicName(s.FilesystemMagic),
		"access_time":                isoSecNsec(s.AccessTimeSec, s.AccessTimeNsec),
		"birth_time":                 isoSecNsec(s.BirthTimeSec, s.BirthTimeNsec),
		"change_time":                isoSecNsec(s.ChangeTimeSec, s.ChangeTimeNsec),
		"modification_time":          isoSecNsec(s.ModificationTimeSec, s.ModificationTimeNsec),
	}
}

func viewNetwork(n *wire.Network) map[string]interface{} {
	m := map[string]interface{}{
		"family":            addressFamilyName(n.Family),
		"socket_type":       socketTypeName(n.SocketType),
		"protocol":          protocolName(n.Family, n.Protocol),
		"socket_state":      socketStateName(n.SocketState),
		"protocol_state":    protocolStateName(n.Family, n.ProtocolState),
		"shutdown":          socketShutdownName(n.Shutdown),
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
	}
	if n.Error != 0 {
		m["error"] = errnoName(uint32(n.Error))
	}
	if n.SoftError != 0 {
		m["soft_error"] = errnoName(uint32(n.SoftError))
	}
	m["local"] = viewSocketEndpoint(n.Local)
	m["peer"] = viewSocketEndpoint(n.Peer)
	return m
}

func viewSocketEndpoint(e wire.SocketEndpoint) map[string]interface{} {
	m := map[string]interface{}{
		"status": socketEndpointStatusName(e.Status),
		"type":   socketEndpointTypeName(e.Type),
	}
	if e.Error != 0 {
		m["error"] = errnoName(uint32(e.Error))
	}
	if e.Status != wire.SocketEndpointAvailable {
		return m
	}
	abstract := e.Flags&wire.SocketEndpointFlagAbstract != 0
	if e.Type == wire.SocketEndpointUnix {
		m["abstract"] = abstract
	}
	if e.Port != 0 {
		m["port"] = e.Port
	}
	if addr := decodeSocketAddress(e.Type, e.Address, abstract); addr != "" {
		m["address"] = addr
	}
	switch e.Type {
	case wire.SocketEndpointIPv6:
		m["scope_id"] = e.ScopeID
		m["flow_info"] = e.FlowInfo
	case wire.SocketEndpointNetlink:
		m["netlink_pid"] = e.NetlinkPID
		m["netlink_groups"] = e.NetlinkGroups
	}
	return m
}

func viewMemoryMapsObject(o *moduleclient.MemoryMapsObject) map[string]interface{} {
	mappings := make([]map[string]interface{}, 0, len(o.Mappings))
	for _, mm := range o.Mappings {
		mappings = append(mappings, viewMemoryMap(mm))
	}
	return map[string]interface{}{
		"status":          objectStatusName(o.Status),
		"page_size_bytes": o.Metadata.PageSizeBytes,
		"mapping_count":   o.Metadata.MappingCount,
		"mapping_limit":   o.Metadata.MappingLimit,
		"mappings":        mappings,
	}
}

func viewMemoryMap(mm *wire.MemoryMap) map[string]interface{} {
	m := map[string]interface{}{
		"mapping_index":     mm.MappingIndex,
		"mapping_type":      mappingTypeName(mm.MappingType),
		"start_address":     hexU64(mm.StartAddress),
		"end_address":       hexU64(mm.EndAddress),
		"page_offset":       mm.PageOffset,
		"mapping_page_size": mm.MappingPageSize,
		"deleted":           mm.FileFlags&0x1 != 0,
		"protection_key":    mm.ProtectionKey,
	}
	perm, shared := mappingPermissions(mm.VMFlags)
	m["permissions"] = perm
	m["shared"] = shared
	m["vm_flags"] = flagNames(mm.VMFlags&^0xf, vmFlagBits)

	if mm.ELFObjectID != 0 {
		m["elf_object_id"] = mm.ELFObjectID
	}

	haveName := mm.NameStatus == wire.DataAvailable && mm.Name != ""
	havePath := mm.PathStatus == wire.DataAvailable && mm.Path != ""
	if haveName {
		m["name"] = mm.Name
	}
	if havePath {
		m["path"] = mm.Path
	}
	if mm.NameError != 0 {
		m["name_error"] = errnoName(mm.NameError)
	}
	if mm.PathError != 0 {
		m["path_error"] = errnoName(mm.PathError)
	}
	if !haveName && !havePath {
		m["name_kind"] = mappingNameKindName(mm.NameKind)
	}

	if mm.FileMetadataStatus == wire.DataAvailable {
		m["file_device_major"] = mm.FileDeviceMajor
		m["file_device_minor"] = mm.FileDeviceMinor
		m["file_mode"] = unixMode(mm.FileMode)
		m["file_inode"] = mm.FileInode
		m["file_size"] = mm.FileSize
		m["file_mtime"] = isoNanos(mm.FileMtimeNS)
		m["file_ctime"] = isoNanos(mm.FileCtimeNS)
		m["file_filesystem"] = fsMagicName(mm.FileFilesystemMagic)
	} else if mm.FileMetadataError != 0 {
		m["file_metadata_error"] = errnoName(mm.FileMetadataError)
	}
	return m
}

// mappingPermissions splits a mapping's VMFlags into the compact,
// self-evident "rwx" triplet and the one genuinely cryptic bit (private vs
// shared) promoted to its own named boolean -- the concrete example that
// drove this whole redesign.
func mappingPermissions(vmFlags uint64) (string, bool) {
	perm := []byte{'-', '-', '-'}
	if vmFlags&0x1 != 0 {
		perm[0] = 'r'
	}
	if vmFlags&0x2 != 0 {
		perm[1] = 'w'
	}
	if vmFlags&0x4 != 0 {
		perm[2] = 'x'
	}
	return string(perm), vmFlags&0x8 != 0
}

// elfSymbolCatalog interns ELF symbol names into a single snapshot-wide
// table, referenced by index instead of repeated inline. Symbol names repeat
// enormously across shared libraries (every consumer of libc imports
// "malloc", "printf", ...), so on a system with many mapped shared objects
// this dwarfs every other size lever in the schema -- interning it is what
// keeps the ELF section from dominating the whole snapshot's size.
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
// symbol.
type elfSymbolGroupKey struct {
	symbolType   string
	binding      string
	visibility   string
	sectionIndex uint16
}

// viewELFSymbolGroups renders one kind (import or export) of a file's
// symbol table as v8's grouped shape: a symbol itself costs one interned
// name index (plus value/size_bytes for a defined export), never a
// duplicated string or a repeated type/binding/visibility triple.
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
			entry["value"] = hexU64(s.Value)
			entry["size_bytes"] = s.Size
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

func viewELFObject(o *moduleclient.ELFObject, catalog *elfSymbolCatalog) map[string]interface{} {
	strs := make([]map[string]interface{}, 0, len(o.Strings))
	for _, s := range o.Strings {
		strs = append(strs, viewELFString(s))
	}
	m := viewELFMetadata(&o.Metadata)
	m["status"] = objectStatusName(o.Status)
	m["strings"] = strs
	m["imports"] = map[string]interface{}{
		"groups": viewELFSymbolGroups(o.Symbols, catalog, wire.ELFSymbolImport, false),
	}
	m["exports"] = map[string]interface{}{
		"groups": viewELFSymbolGroups(o.Symbols, catalog, wire.ELFSymbolExport, true),
	}
	return m
}

func viewELFMetadata(md *wire.ELFMetadata) map[string]interface{} {
	return map[string]interface{}{
		"elf_class":                 elfClassName(md.ELFClass),
		"data_encoding":             elfDataEncodingName(md.DataEncoding),
		"os_abi":                    elfOSABIName(md.OSABI),
		"abi_version":               md.ABIVersion,
		"object_type":               elfObjectTypeName(md.ObjectType),
		"machine":                   elfMachineName(md.Machine),
		"elf_version":               md.ELFVersion,
		"flags":                     md.Flags,
		"program_header_count":      md.ProgramHeaderCount,
		"section_header_count":      md.SectionHeaderCount,
		"string_record_count":       md.StringRecordCount,
		"symbol_record_count":       md.SymbolRecordCount,
		"import_count":              md.ImportCount,
		"export_count":              md.ExportCount,
		"entry_address":             hexU64(md.EntryAddress),
		"program_header_offset":     md.ProgramHeaderOffset,
		"section_header_offset":     md.SectionHeaderOffset,
		"program_header_entry_size": md.ProgramHeaderEntrySize,
		"section_header_entry_size": md.SectionHeaderEntrySize,
		"section_name_index":        md.SectionNameIndex,
		"file_device_major":         md.FileDeviceMajor,
		"file_device_minor":         md.FileDeviceMinor,
		"file_mode":                 unixMode(md.FileMode),
		"file_inode":                md.FileInode,
		"file_size":                 md.FileSize,
		"file_mtime":                isoNanos(md.FileMtimeNS),
		"file_ctime":                isoNanos(md.FileCtimeNS),
		"file_filesystem":           fsMagicName(md.FileFilesystemMagic),
	}
}

func viewELFString(s *wire.ELFString) map[string]interface{} {
	return map[string]interface{}{
		"string_index": s.StringIndex,
		"string_kind":  elfStringKindName(s.StringKind),
		"byte_length":  s.ByteLength,
		"value":        decodeELFStringValue(s.StringKind, s.Bytes),
	}
}

// socketShutdownName renders a socket's shutdown state (SHUT_RD/SHUT_WR low
// 2 bits) as a string rather than a raw bitmask number.
func socketShutdownName(v uint16) string {
	switch v & 0x3 {
	case 1:
		return "read"
	case 2:
		return "write"
	case 3:
		return "both"
	default:
		return "none"
	}
}

// --- top-level snapshot assembly ---

// apiProcessSnapshot builds the API response directly from the already-fully
// -decoded view functions above -- this is the "final data" the responsibility
// boundary calls for: every enum/flag/timestamp/permission is a real value
// by the time it leaves this function, and the TUI (browse.go) only arranges
// and displays it.
func apiProcessSnapshot(snap *moduleclient.ProcessSnapshot) (api.ProcessSnapshot, error) {
	tasks := make([]map[string]interface{}, 0, len(snap.Tasks))
	for _, t := range snap.Tasks {
		tasks = append(tasks, viewTask(t))
	}
	identities := make(map[string]interface{}, len(snap.Identities))
	for id, o := range snap.Identities {
		identities[idKey(id)] = viewIdentity(o)
	}
	securities := make(map[string]interface{}, len(snap.Securities))
	for id, o := range snap.Securities {
		securities[idKey(id)] = viewSecurity(o)
	}
	contexts := make(map[string]interface{}, len(snap.Contexts))
	for id, o := range snap.Contexts {
		contexts[idKey(id)] = viewContext(o)
	}
	resources := make(map[string]interface{}, len(snap.Resources))
	for id, o := range snap.Resources {
		resources[idKey(id)] = viewResources(o)
	}
	environments := make(map[string]interface{}, len(snap.Environments))
	for id, o := range snap.Environments {
		environments[idKey(id)] = viewEnvironment(o)
	}
	memoryMaps := make(map[string]interface{}, len(snap.MemoryMaps))
	for id, o := range snap.MemoryMaps {
		memoryMaps[idKey(id)] = viewMemoryMapsObject(o)
	}
	elfCatalog := newELFSymbolCatalog()
	elfs := make(map[string]interface{}, len(snap.ELFs))
	for id, o := range snap.ELFs {
		elfs[idKey(id)] = viewELFObject(o, elfCatalog)
	}
	elfSymbolNames := elfCatalog.names
	if elfSymbolNames == nil {
		elfSymbolNames = []string{}
	}
	files := make(map[string]interface{}, len(snap.FilesObjects))
	for id, o := range snap.FilesObjects {
		files[idKey(id)] = viewFilesObject(o)
	}

	status := api.Complete
	if snap.Status == wire.SnapshotPartial {
		status = api.Partial
	}
	requested := groupsToAPI(snap.RequestedGroups)
	effective := groupsToAPI(snap.EffectiveGroups)
	supported := groupsToAPI(snap.SupportedGroups)
	return api.ProcessSnapshot{
		Status:          status,
		RequestedGroups: &requested,
		EffectiveGroups: &effective,
		SupportedGroups: &supported,
		TaskCount:       int(snap.TaskCount),
		ObjectCount:     int(snap.ObjectCount),
		Tasks:           tasks,
		Identities:      &identities,
		Securities:      &securities,
		Contexts:        &contexts,
		Resources:       &resources,
		Environments:    &environments,
		MemoryMaps:      &memoryMaps,
		Elfs:            &elfs,
		ElfSymbolNames:  &elfSymbolNames,
		Files:           &files,
	}, nil
}
