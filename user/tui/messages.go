package tui

import (
	"context"
	"encoding/base64"
	"fmt"
	"strings"
	"time"

	tea "charm.land/bubbletea/v2"

	"github.com/GhostDog788/Mapple/user/api"
)

type sessionsLoadedMsg struct {
	sessions    []api.Session
	openBrowser bool // true for an explicit `session list`, false for a background refresh
	err         error
}

type sessionCreatedMsg struct {
	session api.Session
	err     error
}

type sessionDeletedMsg struct {
	id  string
	err error
}

type sessionRenewedMsg struct {
	session api.Session
	err     error
}

type infoResultMsg struct {
	info api.InfoResult
	err  error
}

type echoResultMsg struct {
	data []byte
	err  error
}

type pslistResultMsg struct {
	snapshot api.ProcessSnapshot
	err      error
}

type autoRenewTickMsg struct{}

func apiErr(op string, err error, statusCode int, body []byte) error {
	if err != nil {
		return fmt.Errorf("%s: %w", op, err)
	}
	if statusCode >= 300 {
		return fmt.Errorf("%s: http %d: %s", op, statusCode, string(body))
	}
	return nil
}

// loadSessions fetches the session list and opens the session browser
// (an explicit `session list`). loadSessionsQuiet fetches the same data
// silently, for keeping the id-prefix-resolution cache warm after
// create/delete/renew without interrupting whatever the user is doing.
func (m Model) loadSessions() tea.Cmd      { return m.loadSessionsImpl(true) }
func (m Model) loadSessionsQuiet() tea.Cmd { return m.loadSessionsImpl(false) }

func (m Model) loadSessionsImpl(openBrowser bool) tea.Cmd {
	return func() tea.Msg {
		ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
		defer cancel()
		resp, err := m.client.GetSessionsWithResponse(ctx)
		if err != nil {
			return sessionsLoadedMsg{err: err}
		}
		if resp.JSON200 == nil {
			return sessionsLoadedMsg{err: apiErr("list sessions", nil, resp.StatusCode(), resp.Body)}
		}
		return sessionsLoadedMsg{sessions: *resp.JSON200, openBrowser: openBrowser}
	}
}

func (m Model) createSession(dest api.Destination) tea.Cmd {
	return func() tea.Msg {
		ctx, cancel := context.WithTimeout(context.Background(), 35*time.Second)
		defer cancel()
		resp, err := m.client.PostSessionsWithResponse(ctx, dest)
		if err != nil {
			return sessionCreatedMsg{err: err}
		}
		if resp.JSON201 == nil {
			msg := "unknown error"
			if resp.JSON400 != nil {
				msg = resp.JSON400.Message
			} else if resp.JSON504 != nil {
				msg = resp.JSON504.Message
			}
			return sessionCreatedMsg{err: fmt.Errorf("create session: http %d: %s", resp.StatusCode(), msg)}
		}
		return sessionCreatedMsg{session: api.Session(*resp.JSON201)}
	}
}

func (m Model) deleteSession(id string) tea.Cmd {
	return func() tea.Msg {
		ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
		defer cancel()
		_, err := m.client.DeleteSessionsIdWithResponse(ctx, id)
		return sessionDeletedMsg{id: id, err: err}
	}
}

func (m Model) renewSession(id string) tea.Cmd {
	return func() tea.Msg {
		ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
		defer cancel()
		resp, err := m.client.PostSessionsIdRenewWithResponse(ctx, id)
		if err != nil {
			return sessionRenewedMsg{err: err}
		}
		if resp.JSON200 == nil {
			return sessionRenewedMsg{err: fmt.Errorf("renew: http %d", resp.StatusCode())}
		}
		return sessionRenewedMsg{session: api.Session(*resp.JSON200)}
	}
}

func (m Model) fetchInfo(id string) tea.Cmd {
	return func() tea.Msg {
		ctx, cancel := context.WithTimeout(context.Background(), 35*time.Second)
		defer cancel()
		resp, err := m.client.PostSessionsIdCommandsInfoWithResponse(ctx, id)
		if err != nil {
			return infoResultMsg{err: err}
		}
		if resp.JSON200 == nil {
			return infoResultMsg{err: commandErr("info", resp.StatusCode(), resp.JSON409, resp.JSON410, resp.JSON404)}
		}
		return infoResultMsg{info: api.InfoResult(*resp.JSON200)}
	}
}

func (m Model) sendEcho(id, text string) tea.Cmd {
	return func() tea.Msg {
		ctx, cancel := context.WithTimeout(context.Background(), 35*time.Second)
		defer cancel()
		resp, err := m.client.PostSessionsIdCommandsEchoWithResponse(ctx, id, api.EchoPayload{Data: []byte(text)})
		if err != nil {
			return echoResultMsg{err: err}
		}
		if resp.JSON200 == nil {
			return echoResultMsg{err: commandErr("echo", resp.StatusCode(), resp.JSON409, resp.JSON410, resp.JSON404)}
		}
		return echoResultMsg{data: resp.JSON200.Data}
	}
}

// fetchPslist requests a pslist snapshot. groups is nil for "everything the
// module supports" (today's default, unchanged); a non-nil slice (built
// from the `command pslist groups` checklist) requests only those groups.
func (m Model) fetchPslist(id string, groups []api.PslistGroup) tea.Cmd {
	return func() tea.Msg {
		ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
		defer cancel()
		var body api.PostSessionsIdCommandsPslistJSONRequestBody
		if len(groups) > 0 {
			body.RequestedGroups = &groups
		}
		resp, err := m.client.PostSessionsIdCommandsPslistWithResponse(ctx, id, body)
		if err != nil {
			return pslistResultMsg{err: err}
		}
		if resp.JSON200 == nil {
			msg := commandErr("pslist", resp.StatusCode(), resp.JSON409, resp.JSON410, resp.JSON404)
			if resp.JSON400 != nil {
				msg = fmt.Errorf("pslist: %s", resp.JSON400.Message)
			}
			return pslistResultMsg{err: msg}
		}
		return pslistResultMsg{snapshot: api.ProcessSnapshot(*resp.JSON200)}
	}
}

func commandErr(op string, status int, busy, broken, notFound *api.Error) error {
	switch {
	case busy != nil:
		return fmt.Errorf("%s: %s", op, busy.Message)
	case broken != nil:
		return fmt.Errorf("%s: %s", op, broken.Message)
	case notFound != nil:
		return fmt.Errorf("%s: %s", op, notFound.Message)
	default:
		return fmt.Errorf("%s: http %d", op, status)
	}
}

// commandNames joins the server's already-decoded supported-command names
// for display -- the server (see server/server.go's own commandNames)
// parses the wire bitmask, the TUI only presents the result.
func commandNames(names []string) string {
	if len(names) == 0 {
		return "(none)"
	}
	return strings.Join(names, ", ")
}

func encodeEchoDisplay(data []byte) string {
	if isPrintable(data) {
		return string(data)
	}
	return base64.StdEncoding.EncodeToString(data)
}

func isPrintable(data []byte) bool {
	for _, b := range data {
		if b < 0x20 && b != '\n' && b != '\t' {
			return false
		}
		if b >= 0x7f {
			return false
		}
	}
	return true
}

func autoRenewTick() tea.Cmd {
	return tea.Tick(5*time.Minute, func(time.Time) tea.Msg { return autoRenewTickMsg{} })
}
