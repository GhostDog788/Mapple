package tui

import (
	"fmt"
	"strconv"
	"strings"

	"charm.land/bubbles/v2/list"
	tea "charm.land/bubbletea/v2"

	"github.com/GhostDog788/Mapple/user/api"
)

func (m Model) handleKey(msg tea.KeyPressMsg) (tea.Model, tea.Cmd) {
	key := msg.String()

	// Global quit, except while a text field would want to consume 'q'/ctrl+c
	// itself (create/echo views forward keys to their textinput first).
	if m.view != viewCreate && m.view != viewEcho {
		if key == "q" || key == "ctrl+c" {
			return m, tea.Quit
		}
	} else if key == "ctrl+c" {
		return m, tea.Quit
	}

	switch m.view {
	case viewSessions:
		return m.handleSessionsKey(msg, key)
	case viewCreate:
		return m.handleCreateKey(msg, key)
	case viewDetail:
		return m.handleDetailKey(key)
	case viewEcho:
		return m.handleEchoKey(msg, key)
	case viewPslist:
		return m.handlePslistKey(msg, key)
	}
	return m, nil
}

func (m Model) handleSessionsKey(raw tea.KeyPressMsg, key string) (tea.Model, tea.Cmd) {
	// While filtering, let the list consume every keystroke itself.
	if m.sessions.FilterState() == list.Filtering {
		var cmd tea.Cmd
		m.sessions, cmd = m.sessions.Update(raw)
		return m, cmd
	}
	switch key {
	case "n":
		m.view = viewCreate
		m.err = nil
		return m, nil
	case "R":
		return m, m.loadSessions()
	case "enter":
		if item, ok := m.sessions.SelectedItem().(sessionItem); ok {
			m.selected = item.Session
			m.haveSel = true
			m.detailText = ""
			m.view = viewDetail
		}
		return m, nil
	case "r":
		if item, ok := m.sessions.SelectedItem().(sessionItem); ok {
			return m, m.renewSession(item.Id)
		}
		return m, nil
	case "d":
		if item, ok := m.sessions.SelectedItem().(sessionItem); ok {
			return m, m.deleteSession(item.Id)
		}
		return m, nil
	}
	var cmd tea.Cmd
	m.sessions, cmd = m.sessions.Update(raw)
	return m, cmd
}

func (m Model) handleCreateKey(raw tea.KeyPressMsg, key string) (tea.Model, tea.Cmd) {
	switch key {
	case "esc":
		m.view = viewSessions
		m.err = nil
		return m, nil
	case "tab":
		if m.createType == api.Tcp {
			m.createType = api.Chardev
			m.createInput.Placeholder = "/dev/mapple"
		} else {
			m.createType = api.Tcp
			m.createInput.Placeholder = "host:port"
		}
		return m, nil
	case "enter":
		dest, err := parseDestination(m.createType, m.createInput.Value())
		if err != nil {
			m.err = err
			return m, nil
		}
		m.err = nil
		return m, m.createSession(dest)
	}
	var cmd tea.Cmd
	m.createInput, cmd = m.createInput.Update(raw)
	return m, cmd
}

func parseDestination(t api.DestinationType, value string) (api.Destination, error) {
	value = strings.TrimSpace(value)
	if value == "" {
		return api.Destination{}, fmt.Errorf("enter a destination")
	}
	if t == api.Tcp {
		host, portStr, ok := strings.Cut(value, ":")
		if !ok {
			return api.Destination{}, fmt.Errorf("expected host:port")
		}
		port, err := strconv.Atoi(portStr)
		if err != nil {
			return api.Destination{}, fmt.Errorf("invalid port %q", portStr)
		}
		return api.Destination{Type: api.Tcp, Host: &host, Port: &port}, nil
	}
	return api.Destination{Type: api.Chardev, Path: &value}, nil
}

func (m Model) handleDetailKey(key string) (tea.Model, tea.Cmd) {
	switch key {
	case "esc":
		m.view = viewSessions
		m.detailText = ""
		m.err = nil
		return m, nil
	case "i":
		return m, m.fetchInfo(m.selected.Id)
	case "e":
		m.echoInput.SetValue("")
		m.echoInput.Focus()
		m.view = viewEcho
		return m, nil
	case "p":
		m.pslistBusy = true
		m.view = viewPslist
		return m, m.fetchPslist(m.selected.Id)
	}
	return m, nil
}

func (m Model) handleEchoKey(raw tea.KeyPressMsg, key string) (tea.Model, tea.Cmd) {
	switch key {
	case "esc":
		m.view = viewDetail
		return m, nil
	case "enter":
		text := m.echoInput.Value()
		return m, m.sendEcho(m.selected.Id, text)
	}
	var cmd tea.Cmd
	m.echoInput, cmd = m.echoInput.Update(raw)
	return m, cmd
}

func (m Model) handlePslistKey(raw tea.KeyPressMsg, key string) (tea.Model, tea.Cmd) {
	if m.pslistList.FilterState() == list.Filtering {
		var cmd tea.Cmd
		m.pslistList, cmd = m.pslistList.Update(raw)
		return m, cmd
	}
	switch key {
	case "esc":
		m.view = viewDetail
		return m, nil
	case "s":
		m.pslistSort = (m.pslistSort + 1) % 3
		return m, m.applyPslistSort()
	}
	var cmd tea.Cmd
	m.pslistList, cmd = m.pslistList.Update(raw)
	return m, cmd
}
