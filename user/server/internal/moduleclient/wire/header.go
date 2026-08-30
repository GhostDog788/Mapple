// Package wire is a hand-port of shared/commands.h's wire protocol v3 byte
// layout. It intentionally has no dependency on the C header; header_test.go
// verifies it against bytes captured from a running kernel module.
package wire

import (
	"encoding/binary"
	"errors"
	"fmt"
)

const (
	Magic      uint32 = 0x4d41504c
	Version    uint16 = 3
	HeaderSize        = 24
)

type Command uint16

const (
	CommandEcho   Command = 1
	CommandPslist Command = 2
	CommandInfo   Command = 3
)

type Flags uint32

const (
	FlagStream Flags = 1 << 0
	KnownFlags       = FlagStream
)

type Status uint32

const (
	StatusOK                 Status = 0
	StatusInvalidArgument    Status = 1
	StatusUnsupportedCommand Status = 2
	StatusBusy               Status = 3
	StatusNotFound           Status = 4
	StatusPermissionDenied   Status = 5
	StatusResourceExhausted  Status = 6
	StatusInternalError      Status = 7
)

var ErrBadMagicOrVersion = errors.New("wire: bad magic or unsupported protocol version")

type Header struct {
	Command     Command
	Flags       Flags
	Status      Status
	PayloadSize uint64
}

func (h Header) Encode() [HeaderSize]byte {
	var buf [HeaderSize]byte
	binary.BigEndian.PutUint32(buf[0:4], Magic)
	binary.BigEndian.PutUint16(buf[4:6], Version)
	binary.BigEndian.PutUint16(buf[6:8], uint16(h.Command))
	binary.BigEndian.PutUint32(buf[8:12], uint32(h.Flags))
	binary.BigEndian.PutUint32(buf[12:16], uint32(h.Status))
	binary.BigEndian.PutUint64(buf[16:24], h.PayloadSize)
	return buf
}

func DecodeHeader(buf []byte) (Header, error) {
	if len(buf) != HeaderSize {
		return Header{}, fmt.Errorf("wire: header must be %d bytes, got %d", HeaderSize, len(buf))
	}
	if binary.BigEndian.Uint32(buf[0:4]) != Magic || binary.BigEndian.Uint16(buf[4:6]) != Version {
		return Header{}, ErrBadMagicOrVersion
	}
	return Header{
		Command:     Command(binary.BigEndian.Uint16(buf[6:8])),
		Flags:       Flags(binary.BigEndian.Uint32(buf[8:12])),
		Status:      Status(binary.BigEndian.Uint32(buf[12:16])),
		PayloadSize: binary.BigEndian.Uint64(buf[16:24]),
	}, nil
}

const InfoResponseSize = 24

type InfoResponse struct {
	SessionID         uint64
	MaxSessions       uint32
	Reserved          uint32
	SupportedCommands uint64
}

func DecodeInfoResponse(buf []byte) (InfoResponse, error) {
	if len(buf) != InfoResponseSize {
		return InfoResponse{}, fmt.Errorf("wire: info response must be %d bytes, got %d", InfoResponseSize, len(buf))
	}
	return InfoResponse{
		SessionID:         binary.BigEndian.Uint64(buf[0:8]),
		MaxSessions:       binary.BigEndian.Uint32(buf[8:12]),
		Reserved:          binary.BigEndian.Uint32(buf[12:16]),
		SupportedCommands: binary.BigEndian.Uint64(buf[16:24]),
	}, nil
}
