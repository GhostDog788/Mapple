package server

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"time"
)

type Identity struct {
	Name        string
	TokenSHA256 [sha256.Size]byte
}

type Config struct {
	ListenAddress    string
	TLSCertFile      string
	TLSKeyFile       string
	HandshakeTimeout time.Duration
	LeaseTimeout     time.Duration
	Identities       []Identity
}

type fileIdentity struct {
	Name        string `json:"name"`
	TokenSHA256 string `json:"token_sha256"`
}

type fileConfig struct {
	ListenAddress           string         `json:"listen_address"`
	TLSCertFile             string         `json:"tls_cert_file"`
	TLSKeyFile              string         `json:"tls_key_file"`
	HandshakeTimeoutSeconds int            `json:"handshake_timeout_seconds"`
	LeaseTimeoutMinutes     int            `json:"lease_timeout_minutes"`
	Identities              []fileIdentity `json:"identities"`
}

func LoadConfig(path string) (*Config, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("server: read config %s: %w", path, err)
	}
	var fc fileConfig
	if err := json.Unmarshal(data, &fc); err != nil {
		return nil, fmt.Errorf("server: parse config %s: %w", path, err)
	}
	if fc.ListenAddress == "" {
		return nil, fmt.Errorf("server: config %s: listen_address is required", path)
	}
	if fc.TLSCertFile == "" || fc.TLSKeyFile == "" {
		return nil, fmt.Errorf("server: config %s: tls_cert_file and tls_key_file are required", path)
	}
	if len(fc.Identities) == 0 {
		return nil, fmt.Errorf("server: config %s: at least one identity is required", path)
	}
	handshakeTimeout := 30 * time.Second
	if fc.HandshakeTimeoutSeconds > 0 {
		handshakeTimeout = time.Duration(fc.HandshakeTimeoutSeconds) * time.Second
	}
	leaseTimeout := 15 * time.Minute
	if fc.LeaseTimeoutMinutes > 0 {
		leaseTimeout = time.Duration(fc.LeaseTimeoutMinutes) * time.Minute
	}
	identities := make([]Identity, 0, len(fc.Identities))
	for _, fi := range fc.Identities {
		if fi.Name == "" {
			return nil, fmt.Errorf("server: config %s: identity with empty name", path)
		}
		raw, err := hex.DecodeString(fi.TokenSHA256)
		if err != nil || len(raw) != sha256.Size {
			return nil, fmt.Errorf("server: config %s: identity %q: token_sha256 must be a %d-byte hex string",
				path, fi.Name, sha256.Size)
		}
		var digest [sha256.Size]byte
		copy(digest[:], raw)
		identities = append(identities, Identity{Name: fi.Name, TokenSHA256: digest})
	}
	return &Config{
		ListenAddress:    fc.ListenAddress,
		TLSCertFile:      fc.TLSCertFile,
		TLSKeyFile:       fc.TLSKeyFile,
		HandshakeTimeout: handshakeTimeout,
		LeaseTimeout:     leaseTimeout,
		Identities:       identities,
	}, nil
}
