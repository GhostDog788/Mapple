// Package tui is the mapple Bubble Tea TUI. It talks only to mappled over
// the generated HTTPS client (github.com/GhostDog788/Mapple/user/api) --
// it must never import server/internal/moduleclient, and Go's own
// internal/ visibility rule enforces that structurally, not just by
// convention.
package tui

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"

	"github.com/GhostDog788/Mapple/user/api"
)

type Profile struct {
	ServerURL          string `json:"server_url"`
	BearerToken        string `json:"bearer_token"`
	CAFile             string `json:"ca_file,omitempty"`
	InsecureSkipVerify bool   `json:"insecure_skip_verify,omitempty"`
}

// ModuleProfile is a saved `session create` destination -- a name for a
// commonly-used kernel module (transport + host:port, or a chardev path),
// distinct from Profile (which is the TUI's own connection to mappled
// itself). Kept in the same config file, under "module_profiles" so it
// can't collide with Profile's own "profiles" key.
type ModuleProfile struct {
	Transport string `json:"transport"` // "tcp" or "chardev"
	Host      string `json:"host,omitempty"`
	Port      int    `json:"port,omitempty"`
	Path      string `json:"path,omitempty"`
}

// Destination converts a saved module profile into the api.Destination
// `session create` sends to mappled, validating that the fields its own
// transport actually needs are present.
func (p ModuleProfile) Destination() (api.Destination, error) {
	switch p.Transport {
	case "tcp":
		if p.Host == "" || p.Port == 0 {
			return api.Destination{}, fmt.Errorf("tui: module profile has transport \"tcp\" but missing host/port")
		}
		host, port := p.Host, p.Port
		return api.Destination{Type: api.Tcp, Host: &host, Port: &port}, nil
	case "chardev":
		if p.Path == "" {
			return api.Destination{}, fmt.Errorf("tui: module profile has transport \"chardev\" but missing path")
		}
		path := p.Path
		return api.Destination{Type: api.Chardev, Path: &path}, nil
	default:
		return api.Destination{}, fmt.Errorf("tui: module profile has unknown transport %q (want tcp or chardev)", p.Transport)
	}
}

// String renders a module profile the way `session profiles` lists it.
func (p ModuleProfile) String() string {
	switch p.Transport {
	case "tcp":
		return fmt.Sprintf("tcp %s:%d", p.Host, p.Port)
	case "chardev":
		return fmt.Sprintf("chardev %s", p.Path)
	default:
		return fmt.Sprintf("unknown transport %q", p.Transport)
	}
}

// ModuleProfileFromDestination is Destination's inverse -- used by
// `session save-profile` to turn the currently-connected session's own
// destination back into a savable module profile.
func ModuleProfileFromDestination(d api.Destination) ModuleProfile {
	switch d.Type {
	case api.Tcp:
		p := ModuleProfile{Transport: "tcp"}
		if d.Host != nil {
			p.Host = *d.Host
		}
		if d.Port != nil {
			p.Port = *d.Port
		}
		return p
	default:
		p := ModuleProfile{Transport: "chardev"}
		if d.Path != nil {
			p.Path = *d.Path
		}
		return p
	}
}

type ConfigFile struct {
	ActiveProfile  string                   `json:"active_profile"`
	Profiles       map[string]Profile       `json:"profiles"`
	ModuleProfiles map[string]ModuleProfile `json:"module_profiles,omitempty"`
}

func DefaultConfigPath() (string, error) {
	home, err := os.UserHomeDir()
	if err != nil {
		return "", err
	}
	return filepath.Join(home, ".config", "mapple", "config.json"), nil
}

func LoadConfig(path string) (*ConfigFile, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("tui: read config %s: %w", path, err)
	}
	var cf ConfigFile
	if err := json.Unmarshal(data, &cf); err != nil {
		return nil, fmt.Errorf("tui: parse config %s: %w", path, err)
	}
	if len(cf.Profiles) == 0 {
		return nil, fmt.Errorf("tui: config %s defines no profiles", path)
	}
	return &cf, nil
}

// SelectProfile returns the named profile, or the active_profile if name is
// empty.
func (cf *ConfigFile) SelectProfile(name string) (Profile, string, error) {
	if name == "" {
		name = cf.ActiveProfile
	}
	if name == "" {
		return Profile{}, "", fmt.Errorf("tui: no profile name given and no active_profile set")
	}
	profile, ok := cf.Profiles[name]
	if !ok {
		return Profile{}, "", fmt.Errorf("tui: no such profile %q", name)
	}
	return profile, name, nil
}

// SaveModuleProfile adds or overwrites a module profile in the config file
// at path and writes it back. It re-reads the file first rather than
// working off whatever the TUI loaded at startup, so an in-TUI save
// doesn't clobber unrelated edits (or the active_profile/profiles section)
// made to the file since the session began.
func SaveModuleProfile(path, name string, profile ModuleProfile) error {
	data, err := os.ReadFile(path)
	if err != nil {
		return fmt.Errorf("tui: read config %s: %w", path, err)
	}
	var cf ConfigFile
	if err := json.Unmarshal(data, &cf); err != nil {
		return fmt.Errorf("tui: parse config %s: %w", path, err)
	}
	if cf.ModuleProfiles == nil {
		cf.ModuleProfiles = map[string]ModuleProfile{}
	}
	cf.ModuleProfiles[name] = profile
	out, err := json.MarshalIndent(cf, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(path, out, 0o600)
}

// WriteExampleConfig writes a starter config file at path, mode 0600,
// creating parent directories as needed. It refuses to overwrite an
// existing file.
func WriteExampleConfig(path string) error {
	if _, err := os.Stat(path); err == nil {
		return fmt.Errorf("tui: %s already exists, not overwriting", path)
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		return err
	}
	example := ConfigFile{
		ActiveProfile: "dev",
		Profiles: map[string]Profile{
			"dev": {
				ServerURL:          "https://127.0.0.1:8443",
				BearerToken:        "REPLACE_WITH_YOUR_BEARER_TOKEN",
				InsecureSkipVerify: true,
			},
		},
		ModuleProfiles: map[string]ModuleProfile{
			"example-vm": {Transport: "tcp", Host: "127.0.0.1", Port: 4444},
		},
	}
	data, err := json.MarshalIndent(example, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(path, data, 0o600)
}
