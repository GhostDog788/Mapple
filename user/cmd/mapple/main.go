package main

import (
	"flag"
	"fmt"
	"os"

	tea "charm.land/bubbletea/v2"

	"github.com/GhostDog788/Mapple/user/tui"
)

func main() {
	configPath := flag.String("config", "", "path to mapple TUI config (default ~/.config/mapple/config.json)")
	profileName := flag.String("profile", "", "profile name to use (default: active_profile in config)")
	initConfig := flag.Bool("init", false, "write a starter config file and exit")
	flag.Parse()

	path := *configPath
	if path == "" {
		defaultPath, err := tui.DefaultConfigPath()
		if err != nil {
			fmt.Fprintln(os.Stderr, "mapple:", err)
			os.Exit(1)
		}
		path = defaultPath
	}

	if *initConfig {
		if err := tui.WriteExampleConfig(path); err != nil {
			fmt.Fprintln(os.Stderr, "mapple:", err)
			os.Exit(1)
		}
		fmt.Printf("wrote starter config to %s -- edit it with your server URL and bearer token\n", path)
		return
	}

	cfg, err := tui.LoadConfig(path)
	if err != nil {
		fmt.Fprintln(os.Stderr, "mapple:", err)
		fmt.Fprintf(os.Stderr, "hint: run `mapple -init` to write a starter config to %s\n", path)
		os.Exit(1)
	}
	profile, name, err := cfg.SelectProfile(*profileName)
	if err != nil {
		fmt.Fprintln(os.Stderr, "mapple:", err)
		os.Exit(1)
	}

	client, err := tui.NewClient(profile)
	if err != nil {
		fmt.Fprintln(os.Stderr, "mapple:", err)
		os.Exit(1)
	}

	model := tui.New(client, name, profile)
	if _, err := tea.NewProgram(model).Run(); err != nil {
		fmt.Fprintln(os.Stderr, "mapple:", err)
		os.Exit(1)
	}
}
