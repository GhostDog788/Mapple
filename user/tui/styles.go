package tui

import (
	"image/color"
	"strings"

	"charm.land/lipgloss/v2"

	"github.com/GhostDog788/Mapple/user/api"
)

var (
	headerStyle  = lipgloss.NewStyle().Bold(true).Foreground(lipgloss.Cyan)
	titleStyle   = lipgloss.NewStyle().Bold(true).Foreground(lipgloss.Cyan)
	cursorStyle  = lipgloss.NewStyle().Bold(true).Reverse(true)
	errorStyle   = lipgloss.NewStyle().Bold(true).Foreground(lipgloss.Red)
	successStyle = lipgloss.NewStyle().Foreground(lipgloss.Green)
	hintStyle    = lipgloss.NewStyle().Foreground(lipgloss.BrightBlack)
)

// sectionRowColor marks a row that drills into another frame (a browseRow
// with push set) -- giving every such row the same color, distinct from the
// plain leaf data rows around it, is what actually breaks up a detail
// view's wall of same-colored text into "here's where you can go" vs
// "here's a value".
var sectionRowColor color.Color = lipgloss.Cyan

// fieldValueColor flags a leaf field row's displayed value when it reads as
// a problem worth noticing (data the kernel couldn't get, or a code this
// build doesn't recognize) -- the same "don't just look uniform" reasoning
// as sectionRowColor, applied to data instead of navigation.
func fieldValueColor(displayed string) color.Color {
	switch {
	case strings.Contains(displayed, "unavailable"),
		strings.Contains(displayed, "unsupported"),
		strings.Contains(displayed, "too_large"),
		strings.Contains(displayed, "raced"),
		strings.Contains(displayed, "unknown"):
		return lipgloss.Yellow
	default:
		return nil
	}
}

// sessionStateColor and taskStateColor return nil (no color override, so a
// row renders in the terminal's default foreground) for states that don't
// warrant calling out visually.
func sessionStateColor(s api.SessionState) color.Color {
	switch s {
	case api.SessionStateReady:
		return lipgloss.Green
	case api.SessionStateBusy:
		return lipgloss.Yellow
	case api.SessionStateBroken:
		return lipgloss.Red
	default:
		return nil
	}
}

// taskStateColor maps a Linux process state letter (from /proc's
// TASK_STATE_TO_CHAR_STR convention) to a color: running is worth
// highlighting, uninterruptible-sleep/stopped are worth a caution color,
// zombie is worth flagging, idle kernel threads are dimmed since they
// dominate a typical task list and rarely matter.
func taskStateColor(state string) color.Color {
	switch state {
	case "R":
		return lipgloss.Green
	case "D", "T":
		return lipgloss.Yellow
	case "Z":
		return lipgloss.Red
	case "I":
		return lipgloss.BrightBlack
	default:
		return nil
	}
}
