package tui

import (
	"fmt"
	"sort"
	"strings"

	"charm.land/bubbles/v2/list"
	"charm.land/bubbles/v2/textinput"
	tea "charm.land/bubbletea/v2"

	"github.com/GhostDog788/Mapple/user/api"
)

type view int

const (
	viewSessions view = iota
	viewCreate
	viewDetail
	viewEcho
	viewPslist
)

type sessionItem struct{ api.Session }

func (s sessionItem) FilterValue() string { return s.Id + " " + string(s.State) }
func (s sessionItem) Title() string {
	dest := ""
	if s.Destination.Type == api.Tcp && s.Destination.Host != nil && s.Destination.Port != nil {
		dest = fmt.Sprintf("tcp %s:%d", *s.Destination.Host, *s.Destination.Port)
	} else if s.Destination.Path != nil {
		dest = "chardev " + *s.Destination.Path
	}
	return fmt.Sprintf("%s  [%s]  %s", s.Id[:8], s.State, dest)
}
func (s sessionItem) Description() string {
	return fmt.Sprintf("created %s, expires %s", s.CreatedAt.Format("15:04:05"), s.ExpiresAt.Format("15:04:05"))
}

type taskItem struct {
	pid, tgid, ppid int
	state           string
	comm            string
	hasIdentity     bool
	exePath         string
}

func (t taskItem) FilterValue() string { return fmt.Sprintf("%d %s", t.pid, t.comm) }
func (t taskItem) Title() string {
	return fmt.Sprintf("%-8d %-20s %s", t.pid, t.comm, t.state)
}
func (t taskItem) Description() string {
	if t.hasIdentity && t.exePath != "" {
		return fmt.Sprintf("tgid=%d ppid=%d  %s", t.tgid, t.ppid, t.exePath)
	}
	return fmt.Sprintf("tgid=%d ppid=%d", t.tgid, t.ppid)
}

const (
	sortByPID = iota
	sortByComm
	sortByState
)

type Model struct {
	client      api.ClientWithResponsesInterface
	profileName string
	profile     Profile

	view   view
	status string
	err    error

	width, height int

	sessions   list.Model
	created    map[string]bool // leases this TUI instance created, for auto-renew
	selected   api.Session
	haveSel    bool
	detailText string

	createType  api.DestinationType
	createInput textinput.Model

	echoInput textinput.Model

	pslistItems []taskItem
	pslistList  list.Model
	pslistSort  int
	pslistBusy  bool
}

func New(client api.ClientWithResponsesInterface, profileName string, profile Profile) Model {
	delegate := list.NewDefaultDelegate()
	sessions := list.New(nil, delegate, 0, 0)
	sessions.Title = "mapple sessions"

	pslist := list.New(nil, delegate, 0, 0)
	pslist.Title = "pslist"

	createInput := textinput.New()
	createInput.Placeholder = "host:port"
	createInput.Focus()

	echoInput := textinput.New()
	echoInput.Placeholder = "text to echo"

	return Model{
		client:      client,
		profileName: profileName,
		profile:     profile,
		view:        viewSessions,
		sessions:    sessions,
		pslistList:  pslist,
		created:     make(map[string]bool),
		createType:  api.Tcp,
		createInput: createInput,
		echoInput:   echoInput,
	}
}

func (m Model) Init() tea.Cmd {
	return tea.Batch(m.loadSessions(), autoRenewTick())
}

func (m Model) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	switch msg := msg.(type) {
	case tea.WindowSizeMsg:
		m.width, m.height = msg.Width, msg.Height
		listHeight := msg.Height - 4
		if listHeight < 3 {
			listHeight = 3
		}
		m.sessions.SetSize(msg.Width, listHeight)
		m.pslistList.SetSize(msg.Width, listHeight)
		return m, nil

	case tea.KeyPressMsg:
		return m.handleKey(msg)

	case sessionsLoadedMsg:
		if msg.err != nil {
			m.err = msg.err
			return m, nil
		}
		m.err = nil
		items := make([]list.Item, len(msg.sessions))
		for i, s := range msg.sessions {
			items[i] = sessionItem{s}
		}
		cmd := m.sessions.SetItems(items)
		return m, cmd

	case sessionCreatedMsg:
		if msg.err != nil {
			m.err = msg.err
			m.view = viewCreate
			return m, nil
		}
		m.created[msg.session.Id] = true
		m.status = "created session " + msg.session.Id[:8]
		m.view = viewSessions
		return m, m.loadSessions()

	case sessionDeletedMsg:
		if msg.err != nil {
			m.err = msg.err
		}
		delete(m.created, msg.id)
		return m, m.loadSessions()

	case sessionRenewedMsg:
		if msg.err != nil {
			m.err = msg.err
			return m, nil
		}
		if m.haveSel && m.selected.Id == msg.session.Id {
			m.selected = msg.session
		}
		return m, m.loadSessions()

	case autoRenewTickMsg:
		var cmds []tea.Cmd
		for id := range m.created {
			cmds = append(cmds, m.renewSession(id))
		}
		cmds = append(cmds, autoRenewTick())
		return m, tea.Batch(cmds...)

	case infoResultMsg:
		if msg.err != nil {
			m.err = msg.err
			return m, nil
		}
		m.err = nil
		m.detailText = fmt.Sprintf("INFO\n  session_id: %d\n  max_sessions: %d\n  supported_commands: %#b",
			msg.info.SessionId, msg.info.MaxSessions, msg.info.SupportedCommands)
		return m, nil

	case echoResultMsg:
		if msg.err != nil {
			m.err = msg.err
			m.view = viewDetail
			return m, nil
		}
		m.err = nil
		m.detailText = "ECHO reply: " + encodeEchoDisplay(msg.data)
		m.view = viewDetail
		return m, nil

	case pslistResultMsg:
		m.pslistBusy = false
		if msg.err != nil {
			m.err = msg.err
			m.view = viewDetail
			return m, nil
		}
		m.err = nil
		m.pslistItems = tasksFromSnapshot(msg.snapshot)
		return m, m.applyPslistSort()
	}
	return m, nil
}

func (m Model) View() tea.View {
	var b strings.Builder
	header := fmt.Sprintf("mapple  profile=%s  server=%s\n", m.profileName, m.profile.ServerURL)
	b.WriteString(header)
	if m.err != nil {
		b.WriteString("error: " + m.err.Error() + "\n")
	} else if m.status != "" {
		b.WriteString(m.status + "\n")
	}
	b.WriteString("\n")

	switch m.view {
	case viewSessions:
		b.WriteString(m.sessions.View())
		b.WriteString("\nn=new  enter=detail  r=renew  d=delete  R=refresh  q=quit\n")
	case viewCreate:
		b.WriteString(fmt.Sprintf("Create session (tab to toggle type)\ntype: %s\n\n%s\n\nenter=submit  esc=cancel\n",
			m.createType, m.createInput.View()))
	case viewDetail:
		b.WriteString(sessionDetailView(m.selected))
		if m.detailText != "" {
			b.WriteString("\n" + m.detailText + "\n")
		}
		b.WriteString("\ni=info  e=echo  p=pslist  esc=back\n")
	case viewEcho:
		b.WriteString("Echo payload:\n\n" + m.echoInput.View() + "\n\nenter=send  esc=cancel\n")
	case viewPslist:
		if m.pslistBusy {
			b.WriteString("loading pslist...\n")
		} else {
			b.WriteString(fmt.Sprintf("sort: %s  (s to cycle, / to filter)\n\n", pslistSortName(m.pslistSort)))
			b.WriteString(m.pslistList.View())
		}
		b.WriteString("\nesc=back\n")
	}
	return tea.NewView(b.String())
}

func sessionDetailView(s api.Session) string {
	dest := ""
	switch {
	case s.Destination.Type == api.Tcp && s.Destination.Host != nil && s.Destination.Port != nil:
		dest = fmt.Sprintf("tcp %s:%d", *s.Destination.Host, *s.Destination.Port)
	case s.Destination.Path != nil:
		dest = "chardev " + *s.Destination.Path
	}
	info := "none"
	if s.Info != nil {
		info = fmt.Sprintf("session_id=%d max_sessions=%d supported_commands=%#b",
			s.Info.SessionId, s.Info.MaxSessions, s.Info.SupportedCommands)
	}
	return fmt.Sprintf("Session %s\n  state:       %s\n  destination: %s\n  created:     %s\n  expires:     %s\n  info:        %s",
		s.Id, s.State, dest, s.CreatedAt.Format("2006-01-02 15:04:05"), s.ExpiresAt.Format("2006-01-02 15:04:05"), info)
}

func pslistSortName(mode int) string {
	switch mode {
	case sortByComm:
		return "comm"
	case sortByState:
		return "state"
	default:
		return "pid"
	}
}

func tasksFromSnapshot(snap api.ProcessSnapshot) []taskItem {
	var identities map[string]interface{}
	if snap.Identities != nil {
		identities = *snap.Identities
	}
	items := make([]taskItem, 0, len(snap.Tasks))
	for _, raw := range snap.Tasks {
		t := taskItem{
			pid:   intField(raw, "PID"),
			tgid:  intField(raw, "TGID"),
			ppid:  intField(raw, "PPID"),
			comm:  stringField(raw, "Comm"),
			state: string(rune(intField(raw, "State"))),
		}
		if idID, ok := raw["IdentityID"]; ok && idID != nil {
			if idFloat, ok := idID.(float64); ok {
				key := fmt.Sprintf("%d", int(idFloat))
				if identity, ok := identities[key].(map[string]interface{}); ok {
					t.hasIdentity = true
					t.exePath = stringField(identity, "ExecutablePath")
				}
			}
		}
		items = append(items, t)
	}
	return items
}

func intField(m map[string]interface{}, key string) int {
	if v, ok := m[key].(float64); ok {
		return int(v)
	}
	return 0
}

func stringField(m map[string]interface{}, key string) string {
	if v, ok := m[key].(string); ok {
		return v
	}
	return ""
}

func (m *Model) applyPslistSort() tea.Cmd {
	items := append([]taskItem(nil), m.pslistItems...)
	switch m.pslistSort {
	case sortByComm:
		sort.Slice(items, func(i, j int) bool { return items[i].comm < items[j].comm })
	case sortByState:
		sort.Slice(items, func(i, j int) bool { return items[i].state < items[j].state })
	default:
		sort.Slice(items, func(i, j int) bool { return items[i].pid < items[j].pid })
	}
	listItems := make([]list.Item, len(items))
	for i, it := range items {
		listItems[i] = it
	}
	return m.pslistList.SetItems(listItems)
}
