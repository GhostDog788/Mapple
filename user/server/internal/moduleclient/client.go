package moduleclient

import (
	"context"
	"errors"
	"fmt"
	"io"
	"sync"
	"sync/atomic"
	"time"

	"github.com/GhostDog788/Mapple/user/server/internal/moduleclient/wire"
)

const connectTimeout = 30 * time.Second

var (
	ErrSessionBusy   = errors.New("moduleclient: session busy")
	ErrSessionBroken = errors.New("moduleclient: session broken")
)

// CommandError is a complete, empty error response, not a framing failure.
type CommandError struct {
	Status wire.Status
}

func (e *CommandError) Error() string {
	return fmt.Sprintf("moduleclient: command rejected, status=%d", e.Status)
}

type InfoResult struct {
	SessionID         uint64
	MaxSessions       uint32
	SupportedCommands uint64
}

type ModuleClient struct {
	t           transport
	cmdMu       sync.Mutex
	broken      atomic.Bool
	initialInfo InfoResult
}

func NewTCP(ctx context.Context, host string, port uint16) (*ModuleClient, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	t, err := dialTCP(host, port)
	if err != nil {
		return nil, err
	}
	return newClient(ctx, t)
}

func NewCharDev(ctx context.Context, path string) (*ModuleClient, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	t, err := openCharDev(path)
	if err != nil {
		return nil, err
	}
	return newClient(ctx, t)
}

// Busy reports whether a command is currently in flight.
func (c *ModuleClient) Busy() bool {
	if !c.cmdMu.TryLock() {
		return true
	}
	c.cmdMu.Unlock()
	return false
}

// Broken reports whether a fatal protocol/transport error has latched this
// session; only Close() is meaningful on it after that.
func (c *ModuleClient) Broken() bool {
	return c.broken.Load()
}

// InitialInfo returns the INFO result captured during the connect handshake,
// so callers don't need a second round trip just to cache it.
func (c *ModuleClient) InitialInfo() InfoResult {
	return c.initialInfo
}

func newClient(ctx context.Context, t transport) (*ModuleClient, error) {
	c := &ModuleClient{t: t}
	deadline := time.Now().Add(connectTimeout)
	if d, ok := ctx.Deadline(); ok && d.Before(deadline) {
		deadline = d
	}
	if err := t.SetDeadline(deadline); err != nil {
		t.Close()
		return nil, fmt.Errorf("moduleclient: set connect deadline: %w", err)
	}
	info, err := c.info()
	if err != nil {
		t.Close()
		return nil, fmt.Errorf("moduleclient: initial INFO verification failed: %w", err)
	}
	c.initialInfo = info
	if err := t.SetDeadline(time.Time{}); err != nil {
		t.Close()
		return nil, fmt.Errorf("moduleclient: clear connect deadline: %w", err)
	}
	return c, nil
}

// Close force-closes the session: it does not take cmdMu, so it can
// interrupt a call blocked in Info/Echo from another goroutine. SetDeadline
// on the real transport (verified against the live chardev) causes the
// blocked Read/Write to return immediately.
func (c *ModuleClient) Close() error {
	c.broken.Store(true)
	_ = c.t.SetDeadline(time.Now())
	return c.t.Close()
}

func (c *ModuleClient) Info(ctx context.Context) (InfoResult, error) {
	if err := ctx.Err(); err != nil {
		return InfoResult{}, err
	}
	if !c.cmdMu.TryLock() {
		return InfoResult{}, ErrSessionBusy
	}
	defer c.cmdMu.Unlock()
	if c.broken.Load() {
		return InfoResult{}, ErrSessionBroken
	}
	result, err := c.info()
	if err != nil {
		c.broken.Store(true)
		return InfoResult{}, err
	}
	return result, nil
}

func (c *ModuleClient) Echo(ctx context.Context, payload []byte) ([]byte, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	if !c.cmdMu.TryLock() {
		return nil, ErrSessionBusy
	}
	defer c.cmdMu.Unlock()
	if c.broken.Load() {
		return nil, ErrSessionBroken
	}
	result, err := c.echo(payload)
	if err != nil {
		c.broken.Store(true)
		return nil, err
	}
	return result, nil
}

func (c *ModuleClient) info() (InfoResult, error) {
	req := wire.Header{Command: wire.CommandInfo}.Encode()
	if _, err := c.t.Write(req[:]); err != nil {
		return InfoResult{}, err
	}
	respHdr, err := c.readResponseHeader(wire.CommandInfo)
	if err != nil {
		return InfoResult{}, err
	}
	if respHdr.PayloadSize != wire.InfoResponseSize {
		return InfoResult{}, fmt.Errorf("moduleclient: info response payload size = %d, want %d",
			respHdr.PayloadSize, wire.InfoResponseSize)
	}
	body := make([]byte, wire.InfoResponseSize)
	if _, err := io.ReadFull(c.t, body); err != nil {
		return InfoResult{}, err
	}
	info, err := wire.DecodeInfoResponse(body)
	if err != nil {
		return InfoResult{}, err
	}
	return InfoResult{
		SessionID:         info.SessionID,
		MaxSessions:       info.MaxSessions,
		SupportedCommands: info.SupportedCommands,
	}, nil
}

func (c *ModuleClient) echo(payload []byte) ([]byte, error) {
	req := wire.Header{Command: wire.CommandEcho, PayloadSize: uint64(len(payload))}.Encode()
	if _, err := c.t.Write(req[:]); err != nil {
		return nil, err
	}
	if len(payload) > 0 {
		if _, err := c.t.Write(payload); err != nil {
			return nil, err
		}
	}
	respHdr, err := c.readResponseHeader(wire.CommandEcho)
	if err != nil {
		return nil, err
	}
	if respHdr.PayloadSize != uint64(len(payload)) {
		return nil, fmt.Errorf("moduleclient: echo response size = %d, want %d", respHdr.PayloadSize, len(payload))
	}
	result := make([]byte, respHdr.PayloadSize)
	if _, err := io.ReadFull(c.t, result); err != nil {
		return nil, err
	}
	return result, nil
}

func (c *ModuleClient) readResponseHeader(want wire.Command) (wire.Header, error) {
	buf := make([]byte, wire.HeaderSize)
	if _, err := io.ReadFull(c.t, buf); err != nil {
		return wire.Header{}, err
	}
	h, err := wire.DecodeHeader(buf)
	if err != nil {
		return wire.Header{}, err
	}
	if h.Command != want {
		return wire.Header{}, fmt.Errorf("moduleclient: unexpected response command %d, want %d", h.Command, want)
	}
	if h.Flags&^wire.KnownFlags != 0 {
		return wire.Header{}, fmt.Errorf("moduleclient: unknown response flags %#x", h.Flags)
	}
	if h.Status != wire.StatusOK {
		if h.Flags != 0 || h.PayloadSize != 0 || h.Status > wire.StatusInternalError {
			return wire.Header{}, fmt.Errorf("moduleclient: malformed error response")
		}
		return h, &CommandError{Status: h.Status}
	}
	return h, nil
}
