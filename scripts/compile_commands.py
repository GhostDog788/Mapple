#!/usr/bin/env python3
"""Small helpers for producing portable VS Code compilation databases."""

import argparse
import json
import shlex
from pathlib import Path


CLANGD_UNSUPPORTED_FLAGS = {
	"-mindirect-branch-register",
	"-fno-allow-store-data-races",
	"-fconserve-stack",
	"-fsanitize=bounds-strict",
	"-mrecord-mcount",
}


def clangd_supports(argument: str) -> bool:
	return (
		argument not in CLANGD_UNSUPPORTED_FLAGS
		and not argument.startswith("-mpreferred-stack-boundary=")
		and not argument.startswith("-mindirect-branch=")
	)


def make_clangd_compatible(entry: dict) -> dict:
	entry = dict(entry)
	arguments = entry.get("arguments")
	if not isinstance(arguments, list):
		command = entry.pop("command", "")
		arguments = shlex.split(command)
	entry["arguments"] = [argument for argument in arguments if clangd_supports(argument)]
	return entry


def write_database(path: Path, entries: list[dict]) -> None:
	path.write_text(json.dumps(entries, indent=2) + "\n", encoding="utf-8")


def single(args: argparse.Namespace) -> None:
	if not args.command:
		raise SystemExit("single requires a compiler command after --")

	write_database(
		args.output,
		[
			{
				"directory": str(args.directory.resolve()),
				"file": str(source.resolve()),
				"arguments": args.command,
			}
			for source in args.source
		],
	)


def remap(args: argparse.Namespace) -> None:
	entries = json.loads(args.database.read_text(encoding="utf-8"))
	old_root = str(args.old_root.resolve())
	new_root = str(args.new_root.resolve())

	for entry in entries:
		old_file = entry.get("file")
		if not isinstance(old_file, str) or not old_file.startswith(old_root + "/"):
			continue

		new_file = new_root + old_file[len(old_root) :]
		entry["file"] = new_file
		if isinstance(entry.get("command"), str):
			entry["command"] = entry["command"].replace(old_file, new_file)
		if isinstance(entry.get("arguments"), list):
			entry["arguments"] = [
				new_file if argument == old_file else argument
				for argument in entry["arguments"]
			]

	write_database(args.database, entries)


def merge(args: argparse.Namespace) -> None:
	entries_by_file = {}
	for database in args.databases:
		if not database.is_file():
			continue
		for entry in json.loads(database.read_text(encoding="utf-8")):
			file = entry.get("file")
			if isinstance(file, str) and Path(file).is_file():
				if args.clangd_compatible:
					entry = make_clangd_compatible(entry)
				entries_by_file[file] = entry

	write_database(args.output, list(entries_by_file.values()))


def kernel_source(args: argparse.Namespace) -> None:
	"""Index source-only distro trees as well as already-built kernel trees.

	Keep real kernel compiler commands when available. For other x86 source
	files, derive the target's include paths/config from a module compiler
	command. These fallback entries are for navigation, never for building.
	"""
	entries = json.loads(args.database.read_text()) if args.database.exists() else []
	entries = [e for e in entries if Path(e["file"]).is_file()]
	if entries:
		# A built kernel already has commands for the actual target config.
		write_database(args.database, entries)
		return
	module = json.loads(args.module_database.read_text())
	module = [e for e in module if Path(e["file"]).is_file() and "/src/" in e["file"]]
	if not module:
		raise SystemExit("no module compiler command available for kernel navigation")
	template = make_clangd_compatible(module[0])
	flags = []
	skip = False
	for flag in template["arguments"]:
		if skip:
			skip = False
			continue
		if flag in ("-o", "-c"):
			# -c has no argument; remove only the flag.
			skip = flag == "-o"
			continue
		if flag == template["file"] or flag.startswith(("-Wp,-MMD,", "-DMODULE", "-DKBUILD_", "-D__KBUILD_")):
			continue
		flags.append(flag)
	flags += ["-I" + str(args.source / "include"), "-I" + str(args.source / "arch/x86/include"),
			  '-DKBUILD_MODNAME="kernel"', '-DKBUILD_BASENAME="kernel"']
	known = {e["file"] for e in entries}
	for source in sorted(args.source.rglob("*.c")):
		relative = source.relative_to(args.source)
		# Other architectures and host tools do not share x86 kernel flags.
		if relative.parts[0] in ("tools", "scripts", "samples", "Documentation", "usr"):
			continue
		if relative.parts[0] == "arch" and relative.parts[1] != "x86":
			continue
		if str(source) not in known:
			entries.append({"directory": template["directory"], "file": str(source),
						"arguments": [*flags, "-c", str(source)]})
	write_database(args.database, entries)


def main() -> None:
	parser = argparse.ArgumentParser()
	subparsers = parser.add_subparsers(required=True)

	single_parser = subparsers.add_parser("single")
	single_parser.add_argument("output", type=Path)
	single_parser.add_argument("directory", type=Path)
	single_parser.add_argument("--source", action="append", type=Path, required=True)
	single_parser.add_argument("command", nargs=argparse.REMAINDER)
	single_parser.set_defaults(handler=single)

	remap_parser = subparsers.add_parser("remap")
	remap_parser.add_argument("database", type=Path)
	remap_parser.add_argument("old_root", type=Path)
	remap_parser.add_argument("new_root", type=Path)
	remap_parser.set_defaults(handler=remap)

	merge_parser = subparsers.add_parser("merge")
	merge_parser.add_argument("--clangd-compatible", action="store_true")
	merge_parser.add_argument("output", type=Path)
	merge_parser.add_argument("databases", nargs="+", type=Path)
	merge_parser.set_defaults(handler=merge)

	kernel_parser = subparsers.add_parser("kernel-source")
	kernel_parser.add_argument("database", type=Path)
	kernel_parser.add_argument("module_database", type=Path)
	kernel_parser.add_argument("source", type=Path)
	kernel_parser.set_defaults(handler=kernel_source)

	args = parser.parse_args()
	args.handler(args)


if __name__ == "__main__":
	main()
