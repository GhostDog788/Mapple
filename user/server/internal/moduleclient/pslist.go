package moduleclient

import (
	"context"
	"errors"
	"fmt"
	"io"

	"github.com/GhostDog788/Mapple/user/server/internal/moduleclient/wire"
)

const (
	GroupIdentity   = wire.GroupIdentity
	GroupSecurity   = wire.GroupSecurity
	GroupFDs        = wire.GroupFDs
	GroupFDsDetails = wire.GroupFDsDetails
	GroupNetwork    = wire.GroupNetwork
	DefaultGroups   = wire.DefaultGroups
)

// ErrPslistGroupsUnsupported is returned when the requested groups bitmap
// includes bits the running kernel module doesn't (yet) support.
type ErrPslistGroupsUnsupported struct {
	SupportedGroups uint64
}

func (e ErrPslistGroupsUnsupported) Error() string {
	return fmt.Sprintf("moduleclient: requested pslist groups not supported; supported=%#x", e.SupportedGroups)
}

type ProcessTask struct {
	Type            wire.TaskType
	PID, TGID, PPID uint32
	StartBoottimeNS uint64
	State           byte
	Comm            string
	IdentityID      *uint64
	SecurityID      *uint64
	FilesID         *uint64
	NetworkID       *uint64
}

type IdentityObject struct {
	Status wire.ObjectStatus
	wire.Identity
}

type SecurityObject struct {
	Status wire.ObjectStatus
	wire.Security
}

type FDEntry struct {
	wire.FD
	Details *wire.FDDetails
	Network *wire.Network
}

type FilesObject struct {
	Status wire.ObjectStatus
	FDs    []*FDEntry
}

type ProcessSnapshot struct {
	Status          wire.SnapshotStatus
	RequestedGroups uint64
	EffectiveGroups uint64
	SupportedGroups uint64
	TaskCount       uint32
	ObjectCount     uint32
	Tasks           []*ProcessTask
	Identities      map[uint64]*IdentityObject
	Securities      map[uint64]*SecurityObject
	FilesObjects    map[uint64]*FilesObject
}

// ProcessSnapshot requests a process/thread snapshot for the given groups
// bitmap (see the Group* constants; 0 means DefaultGroups).
func (c *ModuleClient) ProcessSnapshot(ctx context.Context, groups uint64) (*ProcessSnapshot, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	if groups == 0 {
		groups = DefaultGroups
	}
	if !c.cmdMu.TryLock() {
		return nil, ErrSessionBusy
	}
	defer c.cmdMu.Unlock()
	if c.broken.Load() {
		return nil, ErrSessionBroken
	}
	result, err := c.processSnapshot(groups)
	if err != nil {
		var unsupported ErrPslistGroupsUnsupported
		if !errors.As(err, &unsupported) {
			c.broken.Store(true)
		}
		return nil, err
	}
	return result, nil
}

func (c *ModuleClient) processSnapshot(groups uint64) (*ProcessSnapshot, error) {
	req := wire.Header{Command: wire.CommandPslist, PayloadSize: 8}.Encode()
	if _, err := c.t.Write(req[:]); err != nil {
		return nil, err
	}
	body := wire.PslistRequest{Groups: groups}.Encode()
	if _, err := c.t.Write(body[:]); err != nil {
		return nil, err
	}

	hdrBuf := make([]byte, wire.HeaderSize)
	if _, err := io.ReadFull(c.t, hdrBuf); err != nil {
		return nil, err
	}
	hdr, err := wire.DecodeHeader(hdrBuf)
	if err != nil {
		return nil, err
	}
	if hdr.Command != wire.CommandPslist || hdr.Status != wire.StatusOK {
		return nil, fmt.Errorf("moduleclient: unexpected pslist response header %+v", hdr)
	}

	if hdr.Flags&wire.FlagStream == 0 {
		// Legacy (pre-v3-status) fixed error payload: invalid or
		// unsupported groups. Recoverable, not a broken session.
		if hdr.PayloadSize != wire.PslistErrorResponseSize {
			return nil, fmt.Errorf("moduleclient: pslist non-stream response payload size = %d, want %d",
				hdr.PayloadSize, wire.PslistErrorResponseSize)
		}
		buf := make([]byte, wire.PslistErrorResponseSize)
		if _, err := io.ReadFull(c.t, buf); err != nil {
			return nil, err
		}
		errResp, err := wire.DecodePslistErrorResponse(buf)
		if err != nil {
			return nil, err
		}
		return nil, ErrPslistGroupsUnsupported{SupportedGroups: errResp.SupportedGroups}
	}
	if hdr.PayloadSize != 0 {
		return nil, fmt.Errorf("moduleclient: pslist stream response has nonzero payload_size=%d", hdr.PayloadSize)
	}

	return c.readSnapshotStream()
}

func (c *ModuleClient) readSnapshotStream() (*ProcessSnapshot, error) {
	result := &ProcessSnapshot{
		Identities:   make(map[uint64]*IdentityObject),
		Securities:   make(map[uint64]*SecurityObject),
		FilesObjects: make(map[uint64]*FilesObject),
	}
	tasksByPID := make(map[uint32]*ProcessTask)
	var currentFD *FDEntry

	filesObject := func(id uint64) *FilesObject {
		obj, ok := result.FilesObjects[id]
		if !ok {
			obj = &FilesObject{}
			result.FilesObjects[id] = obj
		}
		return obj
	}

	for {
		sizeBuf := make([]byte, 4)
		if _, err := io.ReadFull(c.t, sizeBuf); err != nil {
			return nil, err
		}
		size := int(sizeBuf[0])<<24 | int(sizeBuf[1])<<16 | int(sizeBuf[2])<<8 | int(sizeBuf[3])
		if size < 16 {
			return nil, fmt.Errorf("moduleclient: pslist record_size=%d smaller than header", size)
		}
		buf := make([]byte, size)
		copy(buf, sizeBuf)
		if _, err := io.ReadFull(c.t, buf[4:]); err != nil {
			return nil, err
		}
		h, rest, err := wire.DecodeRecordHeader(buf)
		if err != nil {
			return nil, err
		}

		switch h.Type {
		case wire.StreamSnapshotBegin:
			b, err := wire.DecodeSnapshotBegin(rest)
			if err != nil {
				return nil, err
			}
			result.RequestedGroups = b.RequestedGroups
			result.EffectiveGroups = b.EffectiveGroups
			result.SupportedGroups = b.SupportedGroups

		case wire.StreamSnapshotEnd:
			e, err := wire.DecodeSnapshotEnd(rest)
			if err != nil {
				return nil, err
			}
			result.Status = e.Status
			result.TaskCount = e.TaskCount
			result.ObjectCount = e.ObjectCount
			return result, nil

		case wire.StreamTask:
			t, err := wire.DecodeTask(rest)
			if err != nil {
				return nil, err
			}
			task := &ProcessTask{
				Type: t.Type, PID: t.PID, TGID: t.TGID, PPID: t.PPID,
				StartBoottimeNS: t.StartBoottimeNS, State: t.State, Comm: t.Comm,
			}
			result.Tasks = append(result.Tasks, task)
			tasksByPID[t.PID] = task

		case wire.StreamTaskObjectLink:
			link, err := wire.DecodeTaskObjectLink(rest)
			if err != nil {
				return nil, err
			}
			task, ok := tasksByPID[link.PID]
			if !ok {
				return nil, fmt.Errorf("moduleclient: task_object_link for unknown pid %d", link.PID)
			}
			id := link.ObjectID
			switch h.Group {
			case wire.GroupIdentity:
				task.IdentityID = &id
			case wire.GroupSecurity:
				task.SecurityID = &id
			case wire.GroupFDs:
				task.FilesID = &id
			case wire.GroupNetwork:
				task.NetworkID = &id
			}

		case wire.StreamIdentity:
			id, err := wire.DecodeIdentity(rest)
			if err != nil {
				return nil, err
			}
			result.Identities[id.ObjectID] = &IdentityObject{Status: wire.ObjectComplete, Identity: id}

		case wire.StreamSecurity:
			s, err := wire.DecodeSecurity(rest)
			if err != nil {
				return nil, err
			}
			result.Securities[s.ObjectID] = &SecurityObject{Status: wire.ObjectComplete, Security: s}

		case wire.StreamFD:
			fd, err := wire.DecodeFD(rest)
			if err != nil {
				return nil, err
			}
			entry := &FDEntry{FD: fd}
			obj := filesObject(fd.ObjectID)
			obj.FDs = append(obj.FDs, entry)
			currentFD = entry

		case wire.StreamFDDetails:
			details, err := wire.DecodeFDDetails(rest)
			if err != nil {
				return nil, err
			}
			if currentFD != nil {
				currentFD.Details = &details
			}

		case wire.StreamNetwork:
			network, err := wire.DecodeNetwork(rest)
			if err != nil {
				return nil, err
			}
			if currentFD != nil {
				currentFD.Network = &network
			}

		case wire.StreamObjectEnd:
			end, err := wire.DecodeObjectEnd(rest)
			if err != nil {
				return nil, err
			}
			switch h.Group {
			case wire.GroupIdentity:
				if obj, ok := result.Identities[end.ObjectID]; ok {
					obj.Status = end.Status
				}
			case wire.GroupSecurity:
				if obj, ok := result.Securities[end.ObjectID]; ok {
					obj.Status = end.Status
				}
			case wire.GroupFDs, wire.GroupNetwork:
				if obj, ok := result.FilesObjects[end.ObjectID]; ok {
					obj.Status = end.Status
				}
			}

		case wire.StreamObjectBegin:
			// No data beyond object_id/type, both already carried by
			// the leaf records themselves; nothing to do here.

		default:
			return nil, fmt.Errorf("moduleclient: unknown pslist stream record type %d", h.Type)
		}
	}
}
