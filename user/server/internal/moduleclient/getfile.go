package moduleclient

import (
	"context"
	"errors"
	"fmt"
	"io"
	"math"
	"strings"
	"sync"

	"github.com/GhostDog788/Mapple/user/server/internal/moduleclient/wire"
)

var ErrInvalidGetfilePath = errors.New("path must start with /, contain no NUL bytes, and be at most 4096 bytes")

// FileDownload owns the command lock until Close. Read it sequentially and
// always close it. Abandoning an incomplete payload closes the module session:
// its unread bytes must never be mistaken for the next command's header.
type FileDownload struct {
	Size       int64
	client     *ModuleClient
	remaining  int64
	stopCancel func()
	closed     bool
	once       sync.Once
}

func (f *FileDownload) Read(p []byte) (int, error) {
	if f.closed {
		return 0, io.ErrClosedPipe
	}
	if f.remaining == 0 {
		return 0, io.EOF
	}
	if int64(len(p)) > f.remaining {
		p = p[:int(f.remaining)]
	}
	n, err := f.client.t.Read(p)
	f.remaining -= int64(n)
	if err == io.EOF {
		if f.remaining > 0 {
			err = io.ErrUnexpectedEOF
		} else {
			err = nil
		}
	}
	if err != nil {
		_ = f.client.Close()
	}
	return n, err
}

func (f *FileDownload) Close() error {
	f.once.Do(func() {
		f.stopCancel()
		if f.remaining != 0 {
			_ = f.client.Close()
		}
		f.closed = true
		f.client.cmdMu.Unlock()
	})
	return nil
}

// Getfile reads only the response header. The caller streams the body, keeping
// memory bounded regardless of the file size. A complete error response keeps
// the session usable; malformed framing, cancellation and truncation do not.
func (c *ModuleClient) Getfile(ctx context.Context, path string) (*FileDownload, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	if !strings.HasPrefix(path, "/") || strings.ContainsRune(path, '\x00') || len(path) > wire.GetfileMaxPathLength {
		return nil, ErrInvalidGetfilePath
	}
	if !c.cmdMu.TryLock() {
		return nil, ErrSessionBusy
	}
	if c.broken.Load() {
		c.cmdMu.Unlock()
		return nil, ErrSessionBroken
	}

	canceled := make(chan struct{})
	stop := context.AfterFunc(ctx, func() {
		_ = c.Close()
		close(canceled)
	})
	stopCancel := func() {
		if !stop() {
			<-canceled
		}
	}
	size, err := c.getfile(path)
	if err != nil {
		stopCancel()
		var rejection *CommandError
		if !errors.As(err, &rejection) {
			_ = c.Close()
		}
		c.cmdMu.Unlock()
		if ctx.Err() != nil {
			return nil, ctx.Err()
		}
		return nil, err
	}
	return &FileDownload{Size: size, remaining: size, client: c, stopCancel: stopCancel}, nil
}

func (c *ModuleClient) getfile(path string) (int64, error) {
	req := wire.Header{Command: wire.CommandGetfile, PayloadSize: uint64(len(path))}.Encode()
	if n, err := c.t.Write(req[:]); err != nil {
		return 0, err
	} else if n != len(req) {
		return 0, io.ErrShortWrite
	}
	if n, err := c.t.Write([]byte(path)); err != nil {
		return 0, err
	} else if n != len(path) {
		return 0, io.ErrShortWrite
	}
	h, err := c.readResponseHeader(wire.CommandGetfile)
	if err != nil {
		return 0, err
	}
	if h.Flags != 0 || h.PayloadSize > math.MaxInt64 {
		return 0, fmt.Errorf("moduleclient: invalid GETFILE response length or flags")
	}
	return int64(h.PayloadSize), nil
}
