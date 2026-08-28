package server

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"path/filepath"
	"sync"
	"time"

	"github.com/GhostDog788/Mapple/user/server/internal/moduleclient"
)

type DestinationType string

const (
	DestinationTCP     DestinationType = "tcp"
	DestinationCharDev DestinationType = "chardev"
)

type Destination struct {
	Type DestinationType
	Host string
	Port int
	Path string
}

func (d Destination) validate() error {
	switch d.Type {
	case DestinationTCP:
		if d.Host == "" || d.Port <= 0 || d.Port > 65535 {
			return fmt.Errorf("tcp destination requires host and a valid port")
		}
	case DestinationCharDev:
		if !filepath.IsAbs(d.Path) {
			return fmt.Errorf("chardev destination requires an absolute path")
		}
	default:
		return fmt.Errorf("unknown destination type %q", d.Type)
	}
	return nil
}

type Lease struct {
	ID          string
	Destination Destination
	Client      *moduleclient.ModuleClient
	CreatedAt   time.Time

	mu        sync.Mutex
	expiresAt time.Time
}

func (l *Lease) ExpiresAt() time.Time {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.expiresAt
}

func (l *Lease) touch(leaseTimeout time.Duration) {
	l.mu.Lock()
	defer l.mu.Unlock()
	l.expiresAt = time.Now().Add(leaseTimeout)
}

func (l *Lease) expired() bool {
	l.mu.Lock()
	defer l.mu.Unlock()
	return time.Now().After(l.expiresAt)
}

type SessionState string

const (
	SessionReady  SessionState = "ready"
	SessionBusy   SessionState = "busy"
	SessionBroken SessionState = "broken"
)

func (l *Lease) State() SessionState {
	switch {
	case l.Client.Broken():
		return SessionBroken
	case l.Client.Busy():
		return SessionBusy
	default:
		return SessionReady
	}
}

type SessionManager struct {
	handshakeTimeout time.Duration
	leaseTimeout     time.Duration

	mu      sync.Mutex
	leases  map[string]*Lease
	stopCh  chan struct{}
	stopped sync.Once
}

func NewSessionManager(handshakeTimeout, leaseTimeout time.Duration) *SessionManager {
	m := &SessionManager{
		handshakeTimeout: handshakeTimeout,
		leaseTimeout:     leaseTimeout,
		leases:           make(map[string]*Lease),
		stopCh:           make(chan struct{}),
	}
	go m.reapLoop()
	return m
}

func newLeaseID() (string, error) {
	var raw [16]byte // 128-bit opaque random lease id
	if _, err := rand.Read(raw[:]); err != nil {
		return "", err
	}
	return hex.EncodeToString(raw[:]), nil
}

func (m *SessionManager) Create(ctx context.Context, dest Destination) (*Lease, error) {
	if err := dest.validate(); err != nil {
		return nil, err
	}
	id, err := newLeaseID()
	if err != nil {
		return nil, err
	}
	handshakeCtx, cancel := context.WithTimeout(ctx, m.handshakeTimeout)
	defer cancel()

	var client *moduleclient.ModuleClient
	switch dest.Type {
	case DestinationTCP:
		client, err = moduleclient.NewTCP(handshakeCtx, dest.Host, uint16(dest.Port))
	case DestinationCharDev:
		client, err = moduleclient.NewCharDev(handshakeCtx, dest.Path)
	}
	if err != nil {
		return nil, err
	}

	lease := &Lease{ID: id, Destination: dest, Client: client, CreatedAt: time.Now()}
	lease.touch(m.leaseTimeout)

	m.mu.Lock()
	m.leases[id] = lease
	m.mu.Unlock()
	return lease, nil
}

func (m *SessionManager) Get(id string) (*Lease, bool) {
	m.mu.Lock()
	defer m.mu.Unlock()
	lease, ok := m.leases[id]
	return lease, ok
}

func (m *SessionManager) List() []*Lease {
	m.mu.Lock()
	defer m.mu.Unlock()
	leases := make([]*Lease, 0, len(m.leases))
	for _, lease := range m.leases {
		leases = append(leases, lease)
	}
	return leases
}

func (m *SessionManager) Renew(id string) (*Lease, bool) {
	lease, ok := m.Get(id)
	if !ok {
		return nil, false
	}
	lease.touch(m.leaseTimeout)
	return lease, true
}

// Delete idempotently force-closes and removes a lease, even while busy.
func (m *SessionManager) Delete(id string) {
	m.mu.Lock()
	lease, ok := m.leases[id]
	delete(m.leases, id)
	m.mu.Unlock()
	if ok {
		lease.Client.Close()
	}
}

func (m *SessionManager) ActiveCount() int {
	m.mu.Lock()
	defer m.mu.Unlock()
	return len(m.leases)
}

// CloseAll force-closes every lease, for daemon shutdown.
func (m *SessionManager) CloseAll() {
	m.mu.Lock()
	leases := make([]*Lease, 0, len(m.leases))
	for _, lease := range m.leases {
		leases = append(leases, lease)
	}
	m.leases = make(map[string]*Lease)
	m.mu.Unlock()
	for _, lease := range leases {
		lease.Client.Close()
	}
}

func (m *SessionManager) Stop() {
	m.stopped.Do(func() { close(m.stopCh) })
}

func (m *SessionManager) reapLoop() {
	ticker := time.NewTicker(30 * time.Second)
	defer ticker.Stop()
	for {
		select {
		case <-m.stopCh:
			return
		case <-ticker.C:
			m.reapExpired()
		}
	}
}

// reapExpired removes idle leases past their expiry. A busy lease is never
// reaped -- normal commands have no timeout, so "busy" legitimately means
// still in use regardless of how long that takes.
func (m *SessionManager) reapExpired() {
	m.mu.Lock()
	var toClose []*Lease
	for id, lease := range m.leases {
		if lease.expired() && !lease.Client.Busy() {
			toClose = append(toClose, lease)
			delete(m.leases, id)
		}
	}
	m.mu.Unlock()
	for _, lease := range toClose {
		lease.Client.Close()
	}
}
