package wire

import (
	"encoding/binary"
	"fmt"
	"strings"
)

// Group bitmap, mirrors shared/commands.h MAPPLE_PSLIST_GROUP_*.
const (
	GroupIdentity    uint64 = 1 << 0
	GroupEnvironment uint64 = 1 << 1
	GroupSecurity    uint64 = 1 << 2
	GroupContext     uint64 = 1 << 3
	GroupResources   uint64 = 1 << 4
	GroupFDs         uint64 = 1 << 5
	GroupFDsDetails  uint64 = 1 << 6
	GroupNetwork     uint64 = 1 << 7
	GroupMemoryMaps  uint64 = 1 << 8
	GroupELF         uint64 = 1 << 9

	KnownGroups = GroupIdentity | GroupEnvironment | GroupSecurity | GroupContext |
		GroupResources | GroupFDs | GroupFDsDetails | GroupNetwork | GroupMemoryMaps | GroupELF
	SupportedGroups = KnownGroups
	DefaultGroups   = SupportedGroups
)

type StreamRecordType uint16

const (
	StreamSnapshotBegin  StreamRecordType = 1
	StreamTask           StreamRecordType = 2
	StreamTaskObjectLink StreamRecordType = 3
	StreamObjectBegin    StreamRecordType = 4
	StreamIdentity       StreamRecordType = 5
	StreamFD             StreamRecordType = 6
	StreamFDDetails      StreamRecordType = 7
	StreamNetwork        StreamRecordType = 8
	StreamObjectEnd      StreamRecordType = 9
	StreamSnapshotEnd    StreamRecordType = 10
	StreamSecurity       StreamRecordType = 11
	StreamContext        StreamRecordType = 12
	StreamResources      StreamRecordType = 13
	StreamEnvironment    StreamRecordType = 14
	StreamMemoryMapsMeta StreamRecordType = 15
	StreamMemoryMap      StreamRecordType = 16
	StreamELFMetadata    StreamRecordType = 17
	StreamELFString      StreamRecordType = 18
	StreamELFSymbol      StreamRecordType = 19
)

type TaskType uint16

const (
	TaskThread  TaskType = 1
	TaskProcess TaskType = 2
	TaskKThread TaskType = 3
)

type ObjectType uint16

const (
	ObjectMM      ObjectType = 1
	ObjectFiles   ObjectType = 2
	ObjectTask    ObjectType = 3
	ObjectELFFile ObjectType = 4
)

type SnapshotStatus uint16

const (
	SnapshotComplete SnapshotStatus = 1
	SnapshotPartial  SnapshotStatus = 2
)

type ObjectStatus uint16

const (
	ObjectComplete    ObjectStatus = 1
	ObjectPartial     ObjectStatus = 2
	ObjectUnavailable ObjectStatus = 3
	ObjectRaced       ObjectStatus = 4
	ObjectTooLarge    ObjectStatus = 5
)

type DataStatus uint16

const (
	DataAvailable   DataStatus = 1
	DataUnavailable DataStatus = 2
	DataTooLarge    DataStatus = 3
	DataRaced       DataStatus = 4
	DataUnsupported DataStatus = 5
)

type CommandLineStatus uint16

const (
	CommandLineAvailable   CommandLineStatus = 1
	CommandLineEmpty       CommandLineStatus = 2
	CommandLineUnavailable CommandLineStatus = 3
	CommandLineTooLarge    CommandLineStatus = 4
)

type ExecutableStatus uint16

const (
	ExecutableAvailable   ExecutableStatus = 1
	ExecutableNone        ExecutableStatus = 2
	ExecutableUnavailable ExecutableStatus = 3
)

const (
	ExecutableFlagDeleted             uint32 = 1 << 0
	ExecutableFlagPathUnavailable     uint32 = 1 << 1
	ExecutableFlagMetadataUnavailable uint32 = 1 << 2
)

type FDKind uint16

const (
	FDRegular   FDKind = 1
	FDDirectory FDKind = 2
	FDCharacter FDKind = 3
	FDBlock     FDKind = 4
	FDPipe      FDKind = 5
	FDSocket    FDKind = 6
	FDSymlink   FDKind = 7
	FDAnonInode FDKind = 8
	FDOther     FDKind = 9
)

const FDDescriptorFlagCloexec uint32 = 1 << 0

type SocketEndpointType uint16

const (
	SocketEndpointNone    SocketEndpointType = 0
	SocketEndpointIPv4    SocketEndpointType = 1
	SocketEndpointIPv6    SocketEndpointType = 2
	SocketEndpointUnix    SocketEndpointType = 3
	SocketEndpointNetlink SocketEndpointType = 4
)

type SocketEndpointStatus uint16

const (
	SocketEndpointAvailable    SocketEndpointStatus = 1
	SocketEndpointUnavailable  SocketEndpointStatus = 2
	SocketEndpointNotConnected SocketEndpointStatus = 3
	SocketEndpointUnsupported  SocketEndpointStatus = 4
)

const SocketEndpointFlagAbstract uint16 = 1 << 0

// RecordHeader is the 16-byte prefix common to every pslist stream record.
type RecordHeader struct {
	Size  uint32
	Type  StreamRecordType
	Group uint64
}

const recordHeaderSize = 16

// DecodeRecordHeader reads the common prefix and returns it along with the
// remaining record-specific bytes ("rest"), sliced from buf. buf must be at
// least Size bytes (the caller reads exactly Size bytes off the wire, using
// the 4-byte size prefix, before calling this).
func DecodeRecordHeader(buf []byte) (RecordHeader, []byte, error) {
	if len(buf) < recordHeaderSize {
		return RecordHeader{}, nil, fmt.Errorf("wire: record header must be at least %d bytes, got %d", recordHeaderSize, len(buf))
	}
	h := RecordHeader{
		Size:  binary.BigEndian.Uint32(buf[0:4]),
		Type:  StreamRecordType(binary.BigEndian.Uint16(buf[4:6])),
		Group: binary.BigEndian.Uint64(buf[8:16]),
	}
	if uint32(len(buf)) != h.Size {
		return RecordHeader{}, nil, fmt.Errorf("wire: record_size=%d but buffer has %d bytes", h.Size, len(buf))
	}
	return h, buf[recordHeaderSize:], nil
}

type SnapshotBegin struct {
	RequestedGroups uint64
	EffectiveGroups uint64
	SupportedGroups uint64
	StartBoottimeNS uint64
}

func DecodeSnapshotBegin(rest []byte) (SnapshotBegin, error) {
	if len(rest) != 32 {
		return SnapshotBegin{}, fmt.Errorf("wire: snapshot_begin body must be 32 bytes, got %d", len(rest))
	}
	return SnapshotBegin{
		RequestedGroups: binary.BigEndian.Uint64(rest[0:8]),
		EffectiveGroups: binary.BigEndian.Uint64(rest[8:16]),
		SupportedGroups: binary.BigEndian.Uint64(rest[16:24]),
		StartBoottimeNS: binary.BigEndian.Uint64(rest[24:32]),
	}, nil
}

type SnapshotEnd struct {
	Status        SnapshotStatus
	TaskCount     uint32
	ObjectCount   uint32
	EndBoottimeNS uint64
}

func DecodeSnapshotEnd(rest []byte) (SnapshotEnd, error) {
	if len(rest) != 24 {
		return SnapshotEnd{}, fmt.Errorf("wire: snapshot_end body must be 24 bytes, got %d", len(rest))
	}
	return SnapshotEnd{
		Status:        SnapshotStatus(binary.BigEndian.Uint16(rest[0:2])),
		TaskCount:     binary.BigEndian.Uint32(rest[4:8]),
		ObjectCount:   binary.BigEndian.Uint32(rest[8:12]),
		EndBoottimeNS: binary.BigEndian.Uint64(rest[16:24]),
	}, nil
}

type Task struct {
	Type            TaskType
	PID             uint32
	TGID            uint32
	PPID            uint32
	StartBoottimeNS uint64
	State           byte
	Comm            string
}

func DecodeTask(rest []byte) (Task, error) {
	if len(rest) != 44 {
		return Task{}, fmt.Errorf("wire: task body must be 44 bytes, got %d", len(rest))
	}
	commEnd := 44
	for commEnd > 28 && rest[commEnd-1] == 0 {
		commEnd--
	}
	return Task{
		Type:            TaskType(binary.BigEndian.Uint16(rest[0:2])),
		PID:             binary.BigEndian.Uint32(rest[4:8]),
		TGID:            binary.BigEndian.Uint32(rest[8:12]),
		PPID:            binary.BigEndian.Uint32(rest[12:16]),
		StartBoottimeNS: binary.BigEndian.Uint64(rest[16:24]),
		State:           rest[24],
		Comm:            string(rest[28:commEnd]),
	}, nil
}

type TaskObjectLink struct {
	PID        uint32
	ObjectType ObjectType
	ObjectID   uint64
}

func DecodeTaskObjectLink(rest []byte) (TaskObjectLink, error) {
	if len(rest) != 16 {
		return TaskObjectLink{}, fmt.Errorf("wire: task_object_link body must be 16 bytes, got %d", len(rest))
	}
	return TaskObjectLink{
		PID:        binary.BigEndian.Uint32(rest[0:4]),
		ObjectType: ObjectType(binary.BigEndian.Uint16(rest[4:6])),
		ObjectID:   binary.BigEndian.Uint64(rest[8:16]),
	}, nil
}

type ObjectBegin struct {
	ObjectID   uint64
	ObjectType ObjectType
}

func DecodeObjectBegin(rest []byte) (ObjectBegin, error) {
	if len(rest) != 16 {
		return ObjectBegin{}, fmt.Errorf("wire: object_begin body must be 16 bytes, got %d", len(rest))
	}
	return ObjectBegin{
		ObjectID:   binary.BigEndian.Uint64(rest[0:8]),
		ObjectType: ObjectType(binary.BigEndian.Uint16(rest[8:10])),
	}, nil
}

type ObjectEnd struct {
	ObjectID    uint64
	ObjectType  ObjectType
	Status      ObjectStatus
	ErrorCode   uint32
	RecordCount uint32
}

func DecodeObjectEnd(rest []byte) (ObjectEnd, error) {
	if len(rest) != 24 {
		return ObjectEnd{}, fmt.Errorf("wire: object_end body must be 24 bytes, got %d", len(rest))
	}
	return ObjectEnd{
		ObjectID:    binary.BigEndian.Uint64(rest[0:8]),
		ObjectType:  ObjectType(binary.BigEndian.Uint16(rest[8:10])),
		Status:      ObjectStatus(binary.BigEndian.Uint16(rest[10:12])),
		ErrorCode:   binary.BigEndian.Uint32(rest[12:16]),
		RecordCount: binary.BigEndian.Uint32(rest[16:20]),
	}, nil
}

type Identity struct {
	ObjectID                  uint64
	CommandLineStatus         CommandLineStatus
	ExecutableStatus          ExecutableStatus
	ExecutableFlags           uint32
	ExecutableDeviceMajor     uint32
	ExecutableDeviceMinor     uint32
	ExecutableMode            uint32
	ExecutableInode           uint64
	ExecutableSize            uint64
	ExecutableMtimeNS         int64
	ExecutableCtimeNS         int64
	ExecutableFilesystemMagic uint64
	CommandLine               string
	ExecutablePath            string
}

const identityFixedSize = 76

func DecodeIdentity(rest []byte) (Identity, error) {
	if len(rest) < identityFixedSize {
		return Identity{}, fmt.Errorf("wire: identity body must be at least %d bytes, got %d", identityFixedSize, len(rest))
	}
	cmdLen := binary.BigEndian.Uint32(rest[16:20])
	pathLen := binary.BigEndian.Uint32(rest[20:24])
	trailing := rest[identityFixedSize:]
	if uint64(len(trailing)) != uint64(cmdLen)+uint64(pathLen) {
		return Identity{}, fmt.Errorf("wire: identity trailing data = %d bytes, want %d+%d", len(trailing), cmdLen, pathLen)
	}
	return Identity{
		ObjectID:                  binary.BigEndian.Uint64(rest[0:8]),
		CommandLineStatus:         CommandLineStatus(binary.BigEndian.Uint16(rest[8:10])),
		ExecutableStatus:          ExecutableStatus(binary.BigEndian.Uint16(rest[10:12])),
		ExecutableFlags:           binary.BigEndian.Uint32(rest[12:16]),
		ExecutableDeviceMajor:     binary.BigEndian.Uint32(rest[24:28]),
		ExecutableDeviceMinor:     binary.BigEndian.Uint32(rest[28:32]),
		ExecutableMode:            binary.BigEndian.Uint32(rest[32:36]),
		ExecutableInode:           binary.BigEndian.Uint64(rest[36:44]),
		ExecutableSize:            binary.BigEndian.Uint64(rest[44:52]),
		ExecutableMtimeNS:         int64(binary.BigEndian.Uint64(rest[52:60])),
		ExecutableCtimeNS:         int64(binary.BigEndian.Uint64(rest[60:68])),
		ExecutableFilesystemMagic: binary.BigEndian.Uint64(rest[68:76]),
		CommandLine:               string(trailing[:cmdLen]),
		ExecutablePath:            string(trailing[cmdLen:]),
	}, nil
}

type Security struct {
	ObjectID                     uint64
	UID, GID                     uint32
	EffectiveUID, EffectiveGID   uint32
	SavedUID, SavedGID           uint32
	FilesystemUID, FilesystemGID uint32
	UserNamespace                uint32
	Securebits                   uint32
	NoNewPrivs                   uint32
	SeccompMode                  uint32
	SeccompFilterCount           uint32
	DumpableStatus               DataStatus
	DumpableReason               uint16
	LSMStatus                    DataStatus
	LSMProviderStatus            DataStatus
	Dumpable                     uint32
	LSMError                     uint32
	LSMSecID                     uint32
	LSMProviderError             uint32
	CapabilityInheritable        uint64
	CapabilityPermitted          uint64
	CapabilityEffective          uint64
	CapabilityBounding           uint64
	CapabilityAmbient            uint64
	SupplementaryGroups          []uint32
	LSMProvider                  string
	LSMContext                   []byte
}

const securityFixedSize = 140

func DecodeSecurity(rest []byte) (Security, error) {
	if len(rest) < securityFixedSize {
		return Security{}, fmt.Errorf("wire: security body must be at least %d bytes, got %d", securityFixedSize, len(rest))
	}
	groupCount := binary.BigEndian.Uint32(rest[84:88])
	lsmProviderLen := binary.BigEndian.Uint32(rest[88:92])
	lsmContextLen := binary.BigEndian.Uint32(rest[92:96])
	trailing := rest[securityFixedSize:]
	groupsBytes := uint64(groupCount) * 4
	if uint64(len(trailing)) != groupsBytes+uint64(lsmProviderLen)+uint64(lsmContextLen) {
		return Security{}, fmt.Errorf("wire: security trailing data = %d bytes, want %d+%d+%d",
			len(trailing), groupsBytes, lsmProviderLen, lsmContextLen)
	}
	groups := make([]uint32, groupCount)
	for i := range groups {
		groups[i] = binary.BigEndian.Uint32(trailing[i*4 : i*4+4])
	}
	provider := trailing[groupsBytes : groupsBytes+uint64(lsmProviderLen)]
	context := trailing[groupsBytes+uint64(lsmProviderLen):]

	return Security{
		ObjectID:              binary.BigEndian.Uint64(rest[0:8]),
		UID:                   binary.BigEndian.Uint32(rest[8:12]),
		GID:                   binary.BigEndian.Uint32(rest[12:16]),
		EffectiveUID:          binary.BigEndian.Uint32(rest[16:20]),
		EffectiveGID:          binary.BigEndian.Uint32(rest[20:24]),
		SavedUID:              binary.BigEndian.Uint32(rest[24:28]),
		SavedGID:              binary.BigEndian.Uint32(rest[28:32]),
		FilesystemUID:         binary.BigEndian.Uint32(rest[32:36]),
		FilesystemGID:         binary.BigEndian.Uint32(rest[36:40]),
		UserNamespace:         binary.BigEndian.Uint32(rest[40:44]),
		Securebits:            binary.BigEndian.Uint32(rest[44:48]),
		NoNewPrivs:            binary.BigEndian.Uint32(rest[48:52]),
		SeccompMode:           binary.BigEndian.Uint32(rest[52:56]),
		SeccompFilterCount:    binary.BigEndian.Uint32(rest[56:60]),
		DumpableStatus:        DataStatus(binary.BigEndian.Uint16(rest[60:62])),
		DumpableReason:        binary.BigEndian.Uint16(rest[62:64]),
		LSMStatus:             DataStatus(binary.BigEndian.Uint16(rest[64:66])),
		LSMProviderStatus:     DataStatus(binary.BigEndian.Uint16(rest[66:68])),
		Dumpable:              binary.BigEndian.Uint32(rest[68:72]),
		LSMError:              binary.BigEndian.Uint32(rest[72:76]),
		LSMSecID:              binary.BigEndian.Uint32(rest[76:80]),
		LSMProviderError:      binary.BigEndian.Uint32(rest[80:84]),
		CapabilityInheritable: binary.BigEndian.Uint64(rest[100:108]),
		CapabilityPermitted:   binary.BigEndian.Uint64(rest[108:116]),
		CapabilityEffective:   binary.BigEndian.Uint64(rest[116:124]),
		CapabilityBounding:    binary.BigEndian.Uint64(rest[124:132]),
		CapabilityAmbient:     binary.BigEndian.Uint64(rest[132:140]),
		SupplementaryGroups:   groups,
		LSMProvider:           string(provider),
		LSMContext:            append([]byte(nil), context...),
	}, nil
}

type FD struct {
	ObjectID        uint64
	FD              uint32
	Kind            FDKind
	PathStatus      DataStatus
	IdentityStatus  DataStatus
	DescriptorFlags uint32
	OpenFlags       uint32
	PathError       uint32
	IdentityError   uint32
	DeviceMajor     uint32
	DeviceMinor     uint32
	Inode           uint64
	MountID         uint64
	Path            string
}

const fdFixedSize = 64

func DecodeFD(rest []byte) (FD, error) {
	if len(rest) < fdFixedSize {
		return FD{}, fmt.Errorf("wire: fd body must be at least %d bytes, got %d", fdFixedSize, len(rest))
	}
	pathLen := binary.BigEndian.Uint32(rest[36:40])
	trailing := rest[fdFixedSize:]
	if uint32(len(trailing)) != pathLen {
		return FD{}, fmt.Errorf("wire: fd trailing data = %d bytes, want %d", len(trailing), pathLen)
	}
	return FD{
		ObjectID:        binary.BigEndian.Uint64(rest[0:8]),
		FD:              binary.BigEndian.Uint32(rest[8:12]),
		Kind:            FDKind(binary.BigEndian.Uint16(rest[12:14])),
		PathStatus:      DataStatus(binary.BigEndian.Uint16(rest[14:16])),
		IdentityStatus:  DataStatus(binary.BigEndian.Uint16(rest[16:18])),
		DescriptorFlags: binary.BigEndian.Uint32(rest[20:24]),
		OpenFlags:       binary.BigEndian.Uint32(rest[24:28]),
		PathError:       binary.BigEndian.Uint32(rest[28:32]),
		IdentityError:   binary.BigEndian.Uint32(rest[32:36]),
		DeviceMajor:     binary.BigEndian.Uint32(rest[40:44]),
		DeviceMinor:     binary.BigEndian.Uint32(rest[44:48]),
		Inode:           binary.BigEndian.Uint64(rest[48:56]),
		MountID:         binary.BigEndian.Uint64(rest[56:64]),
		Path:            string(trailing),
	}, nil
}

type FDStat struct {
	ResultMask              uint32
	Mode                    uint32
	LinkCount               uint32
	BlockSize               uint32
	UID, GID                uint32
	SpecialDeviceMajor      uint32
	SpecialDeviceMinor      uint32
	DirectIOMemoryAlignment uint32
	DirectIOOffsetAlignment uint32
	Size                    int64
	Blocks                  uint64
	Attributes              uint64
	AttributesMask          uint64
	ChangeCookie            uint64
	FilesystemMagic         uint64
	AccessTimeSec           int64
	AccessTimeNsec          uint32
	BirthTimeSec            int64
	BirthTimeNsec           uint32
	ChangeTimeSec           int64
	ChangeTimeNsec          uint32
	ModificationTimeSec     int64
	ModificationTimeNsec    uint32
}

func decodeFDStat(b []byte) FDStat {
	timespec := func(off int) (int64, uint32) {
		return int64(binary.BigEndian.Uint64(b[off : off+8])), binary.BigEndian.Uint32(b[off+8 : off+12])
	}
	accessSec, accessNsec := timespec(88)
	birthSec, birthNsec := timespec(104)
	changeSec, changeNsec := timespec(120)
	modSec, modNsec := timespec(136)
	return FDStat{
		ResultMask:              binary.BigEndian.Uint32(b[0:4]),
		Mode:                    binary.BigEndian.Uint32(b[4:8]),
		LinkCount:               binary.BigEndian.Uint32(b[8:12]),
		BlockSize:               binary.BigEndian.Uint32(b[12:16]),
		UID:                     binary.BigEndian.Uint32(b[16:20]),
		GID:                     binary.BigEndian.Uint32(b[20:24]),
		SpecialDeviceMajor:      binary.BigEndian.Uint32(b[24:28]),
		SpecialDeviceMinor:      binary.BigEndian.Uint32(b[28:32]),
		DirectIOMemoryAlignment: binary.BigEndian.Uint32(b[32:36]),
		DirectIOOffsetAlignment: binary.BigEndian.Uint32(b[36:40]),
		Size:                    int64(binary.BigEndian.Uint64(b[40:48])),
		Blocks:                  binary.BigEndian.Uint64(b[48:56]),
		Attributes:              binary.BigEndian.Uint64(b[56:64]),
		AttributesMask:          binary.BigEndian.Uint64(b[64:72]),
		ChangeCookie:            binary.BigEndian.Uint64(b[72:80]),
		FilesystemMagic:         binary.BigEndian.Uint64(b[80:88]),
		AccessTimeSec:           accessSec,
		AccessTimeNsec:          accessNsec,
		BirthTimeSec:            birthSec,
		BirthTimeNsec:           birthNsec,
		ChangeTimeSec:           changeSec,
		ChangeTimeNsec:          changeNsec,
		ModificationTimeSec:     modSec,
		ModificationTimeNsec:    modNsec,
	}
}

type FDDetails struct {
	ObjectID                uint64
	FD                      uint32
	StatStatus              DataStatus
	FDInfoStatus            DataStatus
	FileMode                uint64
	Position                int64
	Version                 uint64
	CredentialUID           uint32
	CredentialGID           uint32
	CredentialEffectiveUID  uint32
	CredentialEffectiveGID  uint32
	CredentialSavedUID      uint32
	CredentialSavedGID      uint32
	CredentialFilesystemUID uint32
	CredentialFilesystemGID uint32
	AsyncOwnerPID           int32
	AsyncOwnerType          uint16
	AsyncSignal             uint16
	AsyncOwnerUID           uint32
	AsyncOwnerEffectiveUID  uint32
	StatError               uint32
	FDInfoError             uint32
	Stat                    FDStat
	FDInfo                  string
}

const fdDetailsFixedSize = 256

func DecodeFDDetails(rest []byte) (FDDetails, error) {
	if len(rest) < fdDetailsFixedSize {
		return FDDetails{}, fmt.Errorf("wire: fd_details body must be at least %d bytes, got %d", fdDetailsFixedSize, len(rest))
	}
	fdinfoLen := binary.BigEndian.Uint32(rest[96:100])
	trailing := rest[fdDetailsFixedSize:]
	if uint32(len(trailing)) != fdinfoLen {
		return FDDetails{}, fmt.Errorf("wire: fd_details trailing data = %d bytes, want %d", len(trailing), fdinfoLen)
	}
	return FDDetails{
		ObjectID:                binary.BigEndian.Uint64(rest[0:8]),
		FD:                      binary.BigEndian.Uint32(rest[8:12]),
		StatStatus:              DataStatus(binary.BigEndian.Uint16(rest[12:14])),
		FDInfoStatus:            DataStatus(binary.BigEndian.Uint16(rest[14:16])),
		FileMode:                binary.BigEndian.Uint64(rest[16:24]),
		Position:                int64(binary.BigEndian.Uint64(rest[24:32])),
		Version:                 binary.BigEndian.Uint64(rest[32:40]),
		CredentialUID:           binary.BigEndian.Uint32(rest[40:44]),
		CredentialGID:           binary.BigEndian.Uint32(rest[44:48]),
		CredentialEffectiveUID:  binary.BigEndian.Uint32(rest[48:52]),
		CredentialEffectiveGID:  binary.BigEndian.Uint32(rest[52:56]),
		CredentialSavedUID:      binary.BigEndian.Uint32(rest[56:60]),
		CredentialSavedGID:      binary.BigEndian.Uint32(rest[60:64]),
		CredentialFilesystemUID: binary.BigEndian.Uint32(rest[64:68]),
		CredentialFilesystemGID: binary.BigEndian.Uint32(rest[68:72]),
		AsyncOwnerPID:           int32(binary.BigEndian.Uint32(rest[72:76])),
		AsyncOwnerType:          binary.BigEndian.Uint16(rest[76:78]),
		AsyncSignal:             binary.BigEndian.Uint16(rest[78:80]),
		AsyncOwnerUID:           binary.BigEndian.Uint32(rest[80:84]),
		AsyncOwnerEffectiveUID:  binary.BigEndian.Uint32(rest[84:88]),
		StatError:               binary.BigEndian.Uint32(rest[88:92]),
		FDInfoError:             binary.BigEndian.Uint32(rest[92:96]),
		Stat:                    decodeFDStat(rest[104:256]),
		FDInfo:                  string(trailing),
	}, nil
}

type SocketEndpoint struct {
	Status        SocketEndpointStatus
	Type          SocketEndpointType
	Error         int32
	Port          uint16
	Flags         uint16
	ScopeID       uint32
	FlowInfo      uint32
	NetlinkPID    uint32
	NetlinkGroups uint32
	Address       []byte
}

const socketEndpointSize = 32

func decodeSocketEndpoint(b, address []byte) SocketEndpoint {
	return SocketEndpoint{
		Status:        SocketEndpointStatus(binary.BigEndian.Uint16(b[0:2])),
		Type:          SocketEndpointType(binary.BigEndian.Uint16(b[2:4])),
		Error:         int32(binary.BigEndian.Uint32(b[4:8])),
		Port:          binary.BigEndian.Uint16(b[8:10]),
		Flags:         binary.BigEndian.Uint16(b[10:12]),
		ScopeID:       binary.BigEndian.Uint32(b[12:16]),
		FlowInfo:      binary.BigEndian.Uint32(b[16:20]),
		NetlinkPID:    binary.BigEndian.Uint32(b[20:24]),
		NetlinkGroups: binary.BigEndian.Uint32(b[24:28]),
		Address:       append([]byte(nil), address...),
	}
}

type Network struct {
	ObjectID         uint64
	FD               uint32
	Family           uint16
	SocketType       uint16
	Protocol         uint16
	SocketState      uint16
	ProtocolState    uint16
	Shutdown         uint16
	Error            int32
	SoftError        int32
	BoundDevice      uint32
	Mark             uint32
	Priority         uint32
	UID              uint32
	NetworkNamespace uint32
	ReceiveBuffer    uint32
	SendBuffer       uint32
	ReceiveQueued    uint32
	SendQueued       uint32
	Drops            uint32
	Local            SocketEndpoint
	Peer             SocketEndpoint
}

const networkFixedSize = 148

func DecodeNetwork(rest []byte) (Network, error) {
	if len(rest) < networkFixedSize {
		return Network{}, fmt.Errorf("wire: network body must be at least %d bytes, got %d", networkFixedSize, len(rest))
	}
	localLen := binary.BigEndian.Uint32(rest[12:16])
	peerLen := binary.BigEndian.Uint32(rest[16:20])
	trailing := rest[networkFixedSize:]
	if uint64(len(trailing)) != uint64(localLen)+uint64(peerLen) {
		return Network{}, fmt.Errorf("wire: network trailing data = %d bytes, want %d+%d", len(trailing), localLen, peerLen)
	}
	s := rest[24:networkFixedSize] // socket_data, 124 bytes
	local := decodeSocketEndpoint(s[60:60+socketEndpointSize], trailing[:localLen])
	peer := decodeSocketEndpoint(s[92:92+socketEndpointSize], trailing[localLen:])
	return Network{
		ObjectID:         binary.BigEndian.Uint64(rest[0:8]),
		FD:               binary.BigEndian.Uint32(rest[8:12]),
		Family:           binary.BigEndian.Uint16(s[0:2]),
		SocketType:       binary.BigEndian.Uint16(s[2:4]),
		Protocol:         binary.BigEndian.Uint16(s[4:6]),
		SocketState:      binary.BigEndian.Uint16(s[6:8]),
		ProtocolState:    binary.BigEndian.Uint16(s[8:10]),
		Shutdown:         binary.BigEndian.Uint16(s[10:12]),
		Error:            int32(binary.BigEndian.Uint32(s[12:16])),
		SoftError:        int32(binary.BigEndian.Uint32(s[16:20])),
		BoundDevice:      binary.BigEndian.Uint32(s[20:24]),
		Mark:             binary.BigEndian.Uint32(s[24:28]),
		Priority:         binary.BigEndian.Uint32(s[28:32]),
		UID:              binary.BigEndian.Uint32(s[32:36]),
		NetworkNamespace: binary.BigEndian.Uint32(s[36:40]),
		ReceiveBuffer:    binary.BigEndian.Uint32(s[40:44]),
		SendBuffer:       binary.BigEndian.Uint32(s[44:48]),
		ReceiveQueued:    binary.BigEndian.Uint32(s[48:52]),
		SendQueued:       binary.BigEndian.Uint32(s[52:56]),
		Drops:            binary.BigEndian.Uint32(s[56:60]),
		Local:            local,
		Peer:             peer,
	}, nil
}

type EnvironmentStatus uint16

const (
	EnvironmentAvailable   EnvironmentStatus = 1
	EnvironmentEmpty       EnvironmentStatus = 2
	EnvironmentUnavailable EnvironmentStatus = 3
	EnvironmentTooLarge    EnvironmentStatus = 4
	EnvironmentRaced       EnvironmentStatus = 5
)

type MemoryReason uint16

const (
	MemoryReasonNone MemoryReason = 0
	MemoryReasonNoMM MemoryReason = 1
)

type MappingType uint16

const (
	MappingAnonymous MappingType = 1
	MappingFile      MappingType = 2
	MappingSpecial   MappingType = 3
)

type MappingNameKind uint16

const (
	MappingNameNone            MappingNameKind = 0
	MappingNameHeap            MappingNameKind = 1
	MappingNameStack           MappingNameKind = 2
	MappingNameAnonymous       MappingNameKind = 3
	MappingNameSharedAnonymous MappingNameKind = 4
	MappingNameSpecial         MappingNameKind = 5
)

type ELFStringKind uint16

const (
	ELFStringInterpreter ELFStringKind = 1
	ELFStringNeeded      ELFStringKind = 2
	ELFStringSoname      ELFStringKind = 3
	ELFStringRpath       ELFStringKind = 4
	ELFStringRunpath     ELFStringKind = 5
	ELFStringBuildID     ELFStringKind = 6
)

type ELFSymbolKind uint16

const (
	ELFSymbolImport ELFSymbolKind = 1
	ELFSymbolExport ELFSymbolKind = 2
)

type Context struct {
	ObjectID                  uint64
	CWDStatus                 DataStatus
	RootStatus                DataStatus
	CgroupStatus              DataStatus
	CgroupHostPathStatus      DataStatus
	CgroupNamespacePathStatus DataStatus
	CWDError                  uint32
	RootError                 uint32
	CgroupError               uint32
	CgroupHostPathError       uint32
	CgroupNamespacePathError  uint32
	NamespaceAvailableMask    uint32
	NamespaceUnsupportedMask  uint32
	UserNamespace             uint32
	UTSNamespace              uint32
	IPCNamespace              uint32
	PIDNamespaceActive        uint32
	PIDNamespaceChildren      uint32
	NetworkNamespace          uint32
	CgroupNamespace           uint32
	TimeNamespace             uint32
	TimeNamespaceChildren     uint32
	CgroupHierarchyID         uint32
	CgroupID                  uint64
	CgroupInode               uint64
	CWD                       string
	Root                      string
	CgroupHostPath            string
	CgroupNamespacePath       string
}

const contextFixedSize = 120

func DecodeContext(rest []byte) (Context, error) {
	if len(rest) < contextFixedSize {
		return Context{}, fmt.Errorf("wire: context body must be at least %d bytes, got %d", contextFixedSize, len(rest))
	}
	cwdLen := binary.BigEndian.Uint32(rest[40:44])
	rootLen := binary.BigEndian.Uint32(rest[44:48])
	hostPathLen := binary.BigEndian.Uint32(rest[48:52])
	nsPathLen := binary.BigEndian.Uint32(rest[52:56])
	trailing := rest[contextFixedSize:]
	want := uint64(cwdLen) + uint64(rootLen) + uint64(hostPathLen) + uint64(nsPathLen)
	if uint64(len(trailing)) != want {
		return Context{}, fmt.Errorf("wire: context trailing data = %d bytes, want %d", len(trailing), want)
	}
	off := uint32(0)
	cwd := trailing[off : off+cwdLen]
	off += cwdLen
	root := trailing[off : off+rootLen]
	off += rootLen
	hostPath := trailing[off : off+hostPathLen]
	off += hostPathLen
	nsPath := trailing[off : off+nsPathLen]

	return Context{
		ObjectID:                  binary.BigEndian.Uint64(rest[0:8]),
		CWDStatus:                 DataStatus(binary.BigEndian.Uint16(rest[8:10])),
		RootStatus:                DataStatus(binary.BigEndian.Uint16(rest[10:12])),
		CgroupStatus:              DataStatus(binary.BigEndian.Uint16(rest[12:14])),
		CgroupHostPathStatus:      DataStatus(binary.BigEndian.Uint16(rest[14:16])),
		CgroupNamespacePathStatus: DataStatus(binary.BigEndian.Uint16(rest[16:18])),
		CWDError:                  binary.BigEndian.Uint32(rest[20:24]),
		RootError:                 binary.BigEndian.Uint32(rest[24:28]),
		CgroupError:               binary.BigEndian.Uint32(rest[28:32]),
		CgroupHostPathError:       binary.BigEndian.Uint32(rest[32:36]),
		CgroupNamespacePathError:  binary.BigEndian.Uint32(rest[36:40]),
		NamespaceAvailableMask:    binary.BigEndian.Uint32(rest[56:60]),
		NamespaceUnsupportedMask:  binary.BigEndian.Uint32(rest[60:64]),
		UserNamespace:             binary.BigEndian.Uint32(rest[64:68]),
		UTSNamespace:              binary.BigEndian.Uint32(rest[68:72]),
		IPCNamespace:              binary.BigEndian.Uint32(rest[72:76]),
		PIDNamespaceActive:        binary.BigEndian.Uint32(rest[76:80]),
		PIDNamespaceChildren:      binary.BigEndian.Uint32(rest[80:84]),
		NetworkNamespace:          binary.BigEndian.Uint32(rest[84:88]),
		CgroupNamespace:           binary.BigEndian.Uint32(rest[88:92]),
		TimeNamespace:             binary.BigEndian.Uint32(rest[92:96]),
		TimeNamespaceChildren:     binary.BigEndian.Uint32(rest[96:100]),
		CgroupHierarchyID:         binary.BigEndian.Uint32(rest[100:104]),
		CgroupID:                  binary.BigEndian.Uint64(rest[104:112]),
		CgroupInode:               binary.BigEndian.Uint64(rest[112:120]),
		CWD:                       string(cwd),
		Root:                      string(root),
		CgroupHostPath:            string(hostPath),
		CgroupNamespacePath:       string(nsPath),
	}, nil
}

type Resources struct {
	ObjectID                   uint64
	MemoryStatus               DataStatus
	IOStatus                   DataStatus
	NetworkStatus              DataStatus
	MemoryReason               MemoryReason
	MemoryError                uint32
	IOError                    uint32
	NetworkError               uint32
	CurrentCPU                 uint32
	SchedulingPolicy           uint32
	Priority                   int32
	Nice                       int32
	RealtimePriority           uint32
	PageSizeBytes              uint32
	CPUUserTimeNS              uint64
	CPUSystemTimeNS            uint64
	MinorFaults                uint64
	MajorFaults                uint64
	VoluntaryContextSwitches   uint64
	InvoluntaryContextSwitches uint64
	IOReadCharacters           uint64
	IOWriteCharacters          uint64
	IOReadSyscalls             uint64
	IOWriteSyscalls            uint64
	IOStorageReadBytes         uint64
	IOStorageWriteBytes        uint64
	IOCancelledWriteBytes      uint64
	MemoryVirtualPages         uint64
	MemoryPeakVirtualPages     uint64
	MemoryLockedPages          uint64
	MemoryDataPages            uint64
	MemoryStackPages           uint64
	MemoryExecutablePages      uint64
	MemoryResidentPages        uint64
	MemoryPeakResidentPages    uint64
	MemoryAnonymousPages       uint64
	MemoryFilePages            uint64
	MemorySharedPages          uint64
	MemorySwapEntries          uint64
}

const resourcesFixedSize = 252

func DecodeResources(rest []byte) (Resources, error) {
	if len(rest) != resourcesFixedSize {
		return Resources{}, fmt.Errorf("wire: resources body must be %d bytes, got %d", resourcesFixedSize, len(rest))
	}
	return Resources{
		ObjectID:                   binary.BigEndian.Uint64(rest[0:8]),
		MemoryStatus:               DataStatus(binary.BigEndian.Uint16(rest[8:10])),
		IOStatus:                   DataStatus(binary.BigEndian.Uint16(rest[10:12])),
		NetworkStatus:              DataStatus(binary.BigEndian.Uint16(rest[12:14])),
		MemoryReason:               MemoryReason(binary.BigEndian.Uint16(rest[14:16])),
		MemoryError:                binary.BigEndian.Uint32(rest[16:20]),
		IOError:                    binary.BigEndian.Uint32(rest[20:24]),
		NetworkError:               binary.BigEndian.Uint32(rest[24:28]),
		CurrentCPU:                 binary.BigEndian.Uint32(rest[28:32]),
		SchedulingPolicy:           binary.BigEndian.Uint32(rest[32:36]),
		Priority:                   int32(binary.BigEndian.Uint32(rest[36:40])),
		Nice:                       int32(binary.BigEndian.Uint32(rest[40:44])),
		RealtimePriority:           binary.BigEndian.Uint32(rest[44:48]),
		PageSizeBytes:              binary.BigEndian.Uint32(rest[48:52]),
		CPUUserTimeNS:              binary.BigEndian.Uint64(rest[52:60]),
		CPUSystemTimeNS:            binary.BigEndian.Uint64(rest[60:68]),
		MinorFaults:                binary.BigEndian.Uint64(rest[68:76]),
		MajorFaults:                binary.BigEndian.Uint64(rest[76:84]),
		VoluntaryContextSwitches:   binary.BigEndian.Uint64(rest[84:92]),
		InvoluntaryContextSwitches: binary.BigEndian.Uint64(rest[92:100]),
		IOReadCharacters:           binary.BigEndian.Uint64(rest[100:108]),
		IOWriteCharacters:          binary.BigEndian.Uint64(rest[108:116]),
		IOReadSyscalls:             binary.BigEndian.Uint64(rest[116:124]),
		IOWriteSyscalls:            binary.BigEndian.Uint64(rest[124:132]),
		IOStorageReadBytes:         binary.BigEndian.Uint64(rest[132:140]),
		IOStorageWriteBytes:        binary.BigEndian.Uint64(rest[140:148]),
		IOCancelledWriteBytes:      binary.BigEndian.Uint64(rest[148:156]),
		MemoryVirtualPages:         binary.BigEndian.Uint64(rest[156:164]),
		MemoryPeakVirtualPages:     binary.BigEndian.Uint64(rest[164:172]),
		MemoryLockedPages:          binary.BigEndian.Uint64(rest[172:180]),
		MemoryDataPages:            binary.BigEndian.Uint64(rest[180:188]),
		MemoryStackPages:           binary.BigEndian.Uint64(rest[188:196]),
		MemoryExecutablePages:      binary.BigEndian.Uint64(rest[196:204]),
		MemoryResidentPages:        binary.BigEndian.Uint64(rest[204:212]),
		MemoryPeakResidentPages:    binary.BigEndian.Uint64(rest[212:220]),
		MemoryAnonymousPages:       binary.BigEndian.Uint64(rest[220:228]),
		MemoryFilePages:            binary.BigEndian.Uint64(rest[228:236]),
		MemorySharedPages:          binary.BigEndian.Uint64(rest[236:244]),
		MemorySwapEntries:          binary.BigEndian.Uint64(rest[244:252]),
	}, nil
}

// AvailabilityStatus is deliberately not named "Status": EnvironmentObject
// (moduleclient/pslist.go) embeds this struct alongside its own
// wire.ObjectStatus field, also named "Status", for the object's
// stream-level completeness. Two fields named "Status" at different
// embedding depths means Go's JSON encoder promotes only the shallower one
// (the wrapper's) and silently drops this one, losing the actual
// environment-availability status entirely -- naming it differently avoids
// the collision.
type Environment struct {
	ObjectID           uint64
	AvailabilityStatus EnvironmentStatus
	Flags              uint16
	Error              uint32
	ByteLength         uint32
	EntryCount         uint32
	Raw                []byte
}

const environmentFixedSize = 28

func DecodeEnvironment(rest []byte) (Environment, error) {
	if len(rest) < environmentFixedSize {
		return Environment{}, fmt.Errorf("wire: environment body must be at least %d bytes, got %d", environmentFixedSize, len(rest))
	}
	byteLength := binary.BigEndian.Uint32(rest[16:20])
	trailing := rest[environmentFixedSize:]
	if uint32(len(trailing)) != byteLength {
		return Environment{}, fmt.Errorf("wire: environment trailing data = %d bytes, want %d", len(trailing), byteLength)
	}
	return Environment{
		ObjectID:           binary.BigEndian.Uint64(rest[0:8]),
		AvailabilityStatus: EnvironmentStatus(binary.BigEndian.Uint16(rest[8:10])),
		Flags:              binary.BigEndian.Uint16(rest[10:12]),
		Error:              binary.BigEndian.Uint32(rest[12:16]),
		ByteLength:         byteLength,
		EntryCount:         binary.BigEndian.Uint32(rest[20:24]),
		Raw:                append([]byte(nil), trailing...),
	}, nil
}

// Entries splits the raw NUL-delimited environment block into individual
// "KEY=VALUE" entries, dropping the trailing empty element left by the
// final NUL.
func (e Environment) Entries() []string {
	if len(e.Raw) == 0 {
		return nil
	}
	parts := strings.Split(string(e.Raw), "\x00")
	if len(parts) > 0 && parts[len(parts)-1] == "" {
		parts = parts[:len(parts)-1]
	}
	return parts
}

type MemoryMapsMetadata struct {
	ObjectID      uint64
	PageSizeBytes uint32
	MappingCount  uint32
	MappingLimit  uint32
}

const memoryMapsMetadataFixedSize = 24

func DecodeMemoryMapsMetadata(rest []byte) (MemoryMapsMetadata, error) {
	if len(rest) != memoryMapsMetadataFixedSize {
		return MemoryMapsMetadata{}, fmt.Errorf("wire: memory_maps_metadata body must be %d bytes, got %d", memoryMapsMetadataFixedSize, len(rest))
	}
	return MemoryMapsMetadata{
		ObjectID:      binary.BigEndian.Uint64(rest[0:8]),
		PageSizeBytes: binary.BigEndian.Uint32(rest[8:12]),
		MappingCount:  binary.BigEndian.Uint32(rest[12:16]),
		MappingLimit:  binary.BigEndian.Uint32(rest[16:20]),
	}, nil
}

type MemoryMap struct {
	ObjectID            uint64
	ELFObjectID         uint64
	MappingIndex        uint32
	MappingType         MappingType
	NameKind            MappingNameKind
	NameStatus          DataStatus
	PathStatus          DataStatus
	FileMetadataStatus  DataStatus
	NameError           uint32
	PathError           uint32
	FileMetadataError   uint32
	FileFlags           uint32
	FileDeviceMajor     uint32
	FileDeviceMinor     uint32
	FileMode            uint32
	ProtectionKey       uint32
	StartAddress        uint64
	EndAddress          uint64
	VMFlags             uint64
	PageOffset          uint64
	MappingPageSize     uint64
	FileInode           uint64
	FileSize            uint64
	FileMtimeNS         int64
	FileCtimeNS         int64
	FileFilesystemMagic uint64
	Name                string
	Path                string
}

const memoryMapFixedSize = 152

func DecodeMemoryMap(rest []byte) (MemoryMap, error) {
	if len(rest) < memoryMapFixedSize {
		return MemoryMap{}, fmt.Errorf("wire: memory_map body must be at least %d bytes, got %d", memoryMapFixedSize, len(rest))
	}
	nameLen := binary.BigEndian.Uint32(rest[48:52])
	pathLen := binary.BigEndian.Uint32(rest[52:56])
	trailing := rest[memoryMapFixedSize:]
	if uint64(len(trailing)) != uint64(nameLen)+uint64(pathLen) {
		return MemoryMap{}, fmt.Errorf("wire: memory_map trailing data = %d bytes, want %d+%d", len(trailing), nameLen, pathLen)
	}
	name := trailing[:nameLen]
	path := trailing[nameLen:]

	return MemoryMap{
		ObjectID:            binary.BigEndian.Uint64(rest[0:8]),
		ELFObjectID:         binary.BigEndian.Uint64(rest[8:16]),
		MappingIndex:        binary.BigEndian.Uint32(rest[16:20]),
		MappingType:         MappingType(binary.BigEndian.Uint16(rest[20:22])),
		NameKind:            MappingNameKind(binary.BigEndian.Uint16(rest[22:24])),
		NameStatus:          DataStatus(binary.BigEndian.Uint16(rest[24:26])),
		PathStatus:          DataStatus(binary.BigEndian.Uint16(rest[26:28])),
		FileMetadataStatus:  DataStatus(binary.BigEndian.Uint16(rest[28:30])),
		NameError:           binary.BigEndian.Uint32(rest[32:36]),
		PathError:           binary.BigEndian.Uint32(rest[36:40]),
		FileMetadataError:   binary.BigEndian.Uint32(rest[40:44]),
		FileFlags:           binary.BigEndian.Uint32(rest[44:48]),
		FileDeviceMajor:     binary.BigEndian.Uint32(rest[56:60]),
		FileDeviceMinor:     binary.BigEndian.Uint32(rest[60:64]),
		FileMode:            binary.BigEndian.Uint32(rest[64:68]),
		ProtectionKey:       binary.BigEndian.Uint32(rest[68:72]),
		StartAddress:        binary.BigEndian.Uint64(rest[72:80]),
		EndAddress:          binary.BigEndian.Uint64(rest[80:88]),
		VMFlags:             binary.BigEndian.Uint64(rest[88:96]),
		PageOffset:          binary.BigEndian.Uint64(rest[96:104]),
		MappingPageSize:     binary.BigEndian.Uint64(rest[104:112]),
		FileInode:           binary.BigEndian.Uint64(rest[112:120]),
		FileSize:            binary.BigEndian.Uint64(rest[120:128]),
		FileMtimeNS:         int64(binary.BigEndian.Uint64(rest[128:136])),
		FileCtimeNS:         int64(binary.BigEndian.Uint64(rest[136:144])),
		FileFilesystemMagic: binary.BigEndian.Uint64(rest[144:152]),
		Name:                string(name),
		Path:                string(path),
	}, nil
}

type ELFMetadata struct {
	ObjectID               uint64
	ELFClass               uint8
	DataEncoding           uint8
	OSABI                  uint8
	ABIVersion             uint8
	ObjectType             uint16
	Machine                uint16
	ELFVersion             uint32
	Flags                  uint32
	ProgramHeaderCount     uint32
	SectionHeaderCount     uint32
	StringRecordCount      uint32
	SymbolRecordCount      uint32
	ImportCount            uint32
	ExportCount            uint32
	EntryAddress           uint64
	ProgramHeaderOffset    uint64
	SectionHeaderOffset    uint64
	ProgramHeaderEntrySize uint16
	SectionHeaderEntrySize uint16
	SectionNameIndex       uint16
	FileDeviceMajor        uint32
	FileDeviceMinor        uint32
	FileMode               uint32
	FileInode              uint64
	FileSize               uint64
	FileMtimeNS            int64
	FileCtimeNS            int64
	FileFilesystemMagic    uint64
}

const elfMetadataFixedSize = 136

func DecodeELFMetadata(rest []byte) (ELFMetadata, error) {
	if len(rest) != elfMetadataFixedSize {
		return ELFMetadata{}, fmt.Errorf("wire: elf_metadata body must be %d bytes, got %d", elfMetadataFixedSize, len(rest))
	}
	return ELFMetadata{
		ObjectID:               binary.BigEndian.Uint64(rest[0:8]),
		ELFClass:               rest[8],
		DataEncoding:           rest[9],
		OSABI:                  rest[10],
		ABIVersion:             rest[11],
		ObjectType:             binary.BigEndian.Uint16(rest[12:14]),
		Machine:                binary.BigEndian.Uint16(rest[14:16]),
		ELFVersion:             binary.BigEndian.Uint32(rest[16:20]),
		Flags:                  binary.BigEndian.Uint32(rest[20:24]),
		ProgramHeaderCount:     binary.BigEndian.Uint32(rest[24:28]),
		SectionHeaderCount:     binary.BigEndian.Uint32(rest[28:32]),
		StringRecordCount:      binary.BigEndian.Uint32(rest[32:36]),
		SymbolRecordCount:      binary.BigEndian.Uint32(rest[36:40]),
		ImportCount:            binary.BigEndian.Uint32(rest[40:44]),
		ExportCount:            binary.BigEndian.Uint32(rest[44:48]),
		EntryAddress:           binary.BigEndian.Uint64(rest[48:56]),
		ProgramHeaderOffset:    binary.BigEndian.Uint64(rest[56:64]),
		SectionHeaderOffset:    binary.BigEndian.Uint64(rest[64:72]),
		ProgramHeaderEntrySize: binary.BigEndian.Uint16(rest[72:74]),
		SectionHeaderEntrySize: binary.BigEndian.Uint16(rest[74:76]),
		SectionNameIndex:       binary.BigEndian.Uint16(rest[76:78]),
		FileDeviceMajor:        binary.BigEndian.Uint32(rest[80:84]),
		FileDeviceMinor:        binary.BigEndian.Uint32(rest[84:88]),
		FileMode:               binary.BigEndian.Uint32(rest[88:92]),
		FileInode:              binary.BigEndian.Uint64(rest[96:104]),
		FileSize:               binary.BigEndian.Uint64(rest[104:112]),
		FileMtimeNS:            int64(binary.BigEndian.Uint64(rest[112:120])),
		FileCtimeNS:            int64(binary.BigEndian.Uint64(rest[120:128])),
		FileFilesystemMagic:    binary.BigEndian.Uint64(rest[128:136]),
	}, nil
}

type ELFString struct {
	ObjectID    uint64
	StringIndex uint32
	StringKind  ELFStringKind
	ByteLength  uint32
	Bytes       []byte
}

const elfStringFixedSize = 24

func DecodeELFString(rest []byte) (ELFString, error) {
	if len(rest) < elfStringFixedSize {
		return ELFString{}, fmt.Errorf("wire: elf_string body must be at least %d bytes, got %d", elfStringFixedSize, len(rest))
	}
	byteLength := binary.BigEndian.Uint32(rest[16:20])
	trailing := rest[elfStringFixedSize:]
	if uint32(len(trailing)) != byteLength {
		return ELFString{}, fmt.Errorf("wire: elf_string trailing data = %d bytes, want %d", len(trailing), byteLength)
	}
	return ELFString{
		ObjectID:    binary.BigEndian.Uint64(rest[0:8]),
		StringIndex: binary.BigEndian.Uint32(rest[8:12]),
		StringKind:  ELFStringKind(binary.BigEndian.Uint16(rest[12:14])),
		ByteLength:  byteLength,
		Bytes:       append([]byte(nil), trailing...),
	}, nil
}

type ELFSymbol struct {
	ObjectID     uint64
	SymbolIndex  uint32
	SymbolKind   ELFSymbolKind
	Binding      uint8
	SymbolType   uint8
	Visibility   uint8
	SectionIndex uint16
	NameLength   uint32
	Value        uint64
	Size         uint64
	Name         string
}

const elfSymbolFixedSize = 44

func DecodeELFSymbol(rest []byte) (ELFSymbol, error) {
	if len(rest) < elfSymbolFixedSize {
		return ELFSymbol{}, fmt.Errorf("wire: elf_symbol body must be at least %d bytes, got %d", elfSymbolFixedSize, len(rest))
	}
	nameLength := binary.BigEndian.Uint32(rest[20:24])
	trailing := rest[elfSymbolFixedSize:]
	if uint32(len(trailing)) != nameLength {
		return ELFSymbol{}, fmt.Errorf("wire: elf_symbol trailing data = %d bytes, want %d", len(trailing), nameLength)
	}
	return ELFSymbol{
		ObjectID:     binary.BigEndian.Uint64(rest[0:8]),
		SymbolIndex:  binary.BigEndian.Uint32(rest[8:12]),
		SymbolKind:   ELFSymbolKind(binary.BigEndian.Uint16(rest[12:14])),
		Binding:      rest[14],
		SymbolType:   rest[15],
		Visibility:   rest[16],
		SectionIndex: binary.BigEndian.Uint16(rest[18:20]),
		NameLength:   nameLength,
		Value:        binary.BigEndian.Uint64(rest[28:36]),
		Size:         binary.BigEndian.Uint64(rest[36:44]),
		Name:         string(trailing),
	}, nil
}

// PslistErrorResponse is the legacy (pre-v3-status) fixed error payload
// mapple_handle_pslist sends, with wire Status still OK, when the request's
// groups bitmap is invalid or partially unsupported.
type PslistErrorResponse struct {
	ErrorCode       uint32
	SupportedGroups uint64
}

const PslistErrorResponseSize = 16

func DecodePslistErrorResponse(buf []byte) (PslistErrorResponse, error) {
	if len(buf) != PslistErrorResponseSize {
		return PslistErrorResponse{}, fmt.Errorf("wire: pslist error response must be %d bytes, got %d", PslistErrorResponseSize, len(buf))
	}
	return PslistErrorResponse{
		ErrorCode:       binary.BigEndian.Uint32(buf[0:4]),
		SupportedGroups: binary.BigEndian.Uint64(buf[8:16]),
	}, nil
}

type PslistRequest struct {
	Groups uint64
}

func (r PslistRequest) Encode() [8]byte {
	var buf [8]byte
	binary.BigEndian.PutUint64(buf[:], r.Groups)
	return buf
}
