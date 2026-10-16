#!/usr/bin/env python3
# Moves this loader to a new upstream OpenXR SDK release: replaces the vendored
# headers in include/openxr, and regenerates the exports from the new xr.xml.
# Writes a markdown report of what changed, plus what still needs a person or
# agent to review, for use as a pull request body.
#
# Upstream rearranging its files shouldn't crash this. Each step that fails or
# finds something moved records a problem, the remaining steps still run, and
# the report leads with what went wrong so the PR can say the sync is partial.
#
# Usage: python3 tools/sync_upstream.py --version 1.1.66 [--work DIR] [--report FILE] [--diffs DIR]
#        [--sdk-dir DIR] [--source-dir DIR]   use local copies instead of downloading
# Prints "changed=true|false" and "problems=true|false" as its last two lines, for CI.

import argparse
import difflib
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import traceback
import urllib.request

ROOT        = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADERS     = os.path.join(ROOT, "include", "openxr")
TARBALL_URL = "https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/release-{version}.tar.gz"
SOURCE_URL  = "https://github.com/KhronosGroup/OpenXR-SDK-Source.git"
VERSION_RE  = r"define XR_CURRENT_API_VERSION XR_MAKE_VERSION\((\d+), *(\d+), *(\d+)\)"

# Where things live in an OpenXR-SDK release, searched for by name if they move
SDK_HEADERS   = "include/openxr"
SDK_XML       = "specification/registry/xr.xml"
SDK_CHANGELOG = "CHANGELOG.SDK.md"

# Upstream paths in OpenXR-SDK-Source whose changes may need porting by hand
LOADER_PATHS = ["src/loader", "src/common/filesystem_utils.cpp", "src/common/filesystem_utils.hpp",
                "src/common/platform_utils.hpp", "src/common/hex_and_handles.h", "src/common/object_info.cpp",
                "src/common/object_info.h", "src/scripts/loader_source_generator.py"]
DOC_PATHS    = ["specification/loader"]

# PR bodies are capped at 65536 characters, so long diffs are trimmed and attached as artifacts instead
INLINE_DIFF_LIMIT = 12000

AGENT_PROMPT = """\
You are updating this repository's plain C11 OpenXR loader from upstream OpenXR SDK {old} to {new}. Read CLAUDE.md first, and follow its coding guidelines and testing instructions.

This PR already has the mechanical updates: the vendored headers in include/openxr, and src/exports.gen.* plus the export lists regenerated from the new xr.xml. Your job is the behavioral review.

0. If this PR lists "Sync problems", the mechanical update is incomplete. Fix those first, including updating tools/sync_upstream.py if upstream moved files it depends on, so the next sync works on its own.
1. Go through every upstream change listed in this PR body: the "Loader" changelog entries, the upstream loader source diff (src/loader and the src/common files it uses) between release-{old} and release-{new} in KhronosGroup/OpenXR-SDK-Source, and the loader design doc diff (specification/loader). Read the full diffs through the compare link at the top of this PR, or this workflow run's artifacts, or `git diff release-{old} release-{new} -- src/loader specification/loader` in an OpenXR-SDK-Source clone.
2. Decide for each change whether to port it. Port behavior changes, bug fixes, new loader-level extensions, manifest or discovery changes, and negotiation interface changes. Skip C++-only cleanups, refactors, build system and test-only changes. Upstream is the behavioral reference, except for the deliberate differences listed in CLAUDE.md.
3. Port what's needed into src/ in plain C11, keeping the binary small. No printf or scanf family, use xrl_fmt.
4. Handle anything flagged under "Needs attention". A changed openxr_loader_negotiation.h means new interface or struct versions to support. New core functions need a look at MANUAL and DISPATCH_EXTRA in tools/gen_exports.py. A new major version means updating XRL_MAJOR_STR and the "openxr/1" paths.
5. Fix anything failing under "Test results", then build for Linux, Android and Windows and run upstream's loader_test and the probe differential as CLAUDE.md describes.
6. Leave a comment on this PR listing each upstream change, whether you ported or skipped it, and a one line reason.
"""

def run(*args, cwd=None):
	return subprocess.run(args, cwd=cwd, check=True, text=True, capture_output=True).stdout

def read(path):
	with open(path, encoding="utf-8", newline="") as f:
		return f.read()

def write(path, text):
	os.makedirs(os.path.dirname(path), exist_ok=True)
	with open(path, "w", encoding="utf-8", newline="") as f:
		f.write(text)

def version_key(version):
	return tuple(int(part) for part in version.split("."))

def header_version(text):
	match = re.search(VERSION_RE, text)
	return ".".join(match.groups()) if match else None

def line_delta(old, new):
	added = removed = 0
	for line in difflib.unified_diff(old.splitlines(), new.splitlines(), lineterm="", n=0):
		if   line.startswith("+") and not line.startswith("+++"): added   += 1
		elif line.startswith("-") and not line.startswith("---"): removed += 1
	return added, removed

def export_list(map_text):
	return re.findall(r"^\s+(xr\w+);", map_text, re.M)

def core_functions(reflection_text):
	return set(re.findall(r"_\((\w+), VERSION_\d+_\d+\)", reflection_text))

def fenced(text, lang="diff"):
	if len(text) > INLINE_DIFF_LIMIT:
		text = text[:INLINE_DIFF_LIMIT] + "\n... trimmed, see the workflow artifacts for the full diff\n"
	return f"```{lang}\n{text.rstrip()}\n```\n"

def details(summary, body):
	return f"<details><summary>{summary}</summary>\n\n{body}\n</details>\n"

def changelog_sections(changelog, old, new):
	# Every "## OpenXR SDK X.Y.Z" section newer than old, up to and including new
	result = []
	for section in re.split(r"(?m)^(?=## OpenXR SDK )", changelog):
		match = re.match(r"## OpenXR SDK (\d+\.\d+\.\d+)", section)
		if match and version_key(old) < version_key(match.group(1)) <= version_key(new):
			result.append((match.group(1), section.strip()))
	return result

def loader_entries(section):
	# Changelog bullets that mention the loader, joined with their continuation lines
	entries, current = [], None
	for line in section.splitlines():
		if re.match(r"^\s*- ", line):
			if current: entries.append(current)
			current = line.strip()[2:] if re.search(r"\bloader\b", line, re.I) else None
		elif current is not None and line.startswith("    ") and not line.strip().startswith("("):
			current += " " + line.strip()
	if current: entries.append(current)
	return entries

class Sync:
	def __init__(self):
		self.auto      = []
		self.attention = []
		self.problems  = []
		self.review    = []
		self.entries   = []

	def step(self, description, fn, *args):
		# Runs one part of the sync, turning any failure into a reported problem
		try:
			return fn(*args)
		except subprocess.CalledProcessError as e:
			output = (e.stderr or e.stdout or "").strip()[-1500:]
			self.problems.append(f"{description} failed: `{' '.join(map(str, e.cmd))}` exited {e.returncode}" + (f"\n\n{fenced(output, 'text')}" if output else ""))
		except Exception as e:
			self.problems.append(f"{description} failed: {type(e).__name__}: {e}\n\n" + details("Traceback", fenced(traceback.format_exc(), "text")))
		return None

	def locate(self, base, expected, name, is_dir=False):
		# Finds an upstream file or directory by its usual path, falling back to a search by name
		path = os.path.join(base, expected)
		if os.path.isdir(path) if is_dir else os.path.isfile(path):
			return path
		for dirpath, dirnames, filenames in os.walk(base):
			if (name in dirnames) if is_dir else (name in filenames):
				found = os.path.join(dirpath, name)
				if is_dir and not any(f.endswith(".h") for f in os.listdir(found)): continue
				self.problems.append(f"`{expected}` wasn't in the release, using `{os.path.relpath(found, base)}` instead. Update `tools/sync_upstream.py` if upstream moved it for good.")
				return found
		also = "" if os.path.basename(expected) == name else f" or any `{name}`"
		self.problems.append(f"Couldn't find `{expected}`{also} in the release")
		return None

def fetch_sdk(version, work):
	tar_path = os.path.join(work, f"OpenXR-SDK-{version}.tar.gz")
	with urllib.request.urlopen(TARBALL_URL.format(version=version)) as response, open(tar_path, "wb") as f:
		shutil.copyfileobj(response, f)
	with tarfile.open(tar_path) as tar:
		root = os.path.commonpath(tar.getnames())
		tar.extractall(work, filter="data")
	return os.path.join(work, root)

def fetch_source(old, new, work):
	src = os.path.join(work, "OpenXR-SDK-Source")
	if os.path.exists(src): shutil.rmtree(src)
	run("git", "clone", "--quiet", "--depth", "1", "--branch", f"release-{new}", SOURCE_URL, src)
	run("git", "fetch", "--quiet", "--depth", "1", "origin", "tag", f"release-{old}", cwd=src)
	return src

def update_headers(sync, new_headers, new):
	old_texts = {n: read(os.path.join(HEADERS, n)) for n in os.listdir(HEADERS) if n.endswith(".h")}
	new_texts = {n: read(os.path.join(new_headers, n)) for n in os.listdir(new_headers) if n.endswith(".h")}

	# CMake takes its version from openxr.h, so don't vendor headers that can't provide it
	if "openxr.h" not in new_texts:
		raise RuntimeError("the release's header directory has no openxr.h, left the vendored headers as they were")
	found = header_version(new_texts["openxr.h"])
	if found != new:
		raise RuntimeError(f"the new openxr.h reports version {found or 'nothing CMake can parse'} instead of {new}, left the vendored headers as they were")

	delta = []
	for name in sorted(old_texts.keys() | new_texts.keys()):
		if name not in new_texts:
			os.remove(os.path.join(HEADERS, name))
			sync.attention.append(f"Upstream removed `include/openxr/{name}`, so it's gone from ours too")
		elif old_texts.get(name) != new_texts[name]:
			write(os.path.join(HEADERS, name), new_texts[name])
			added, removed = line_delta(old_texts.get(name, ""), new_texts[name])
			delta.append(f"`{name}` +{added}/-{removed}" + ("" if name in old_texts else " (new)"))
	sync.auto.append(f"Vendored headers in `include/openxr` updated to {new}: " + (", ".join(delta) or "no changes"))

	old_neg, new_neg = old_texts.get("openxr_loader_negotiation.h", ""), new_texts.get("openxr_loader_negotiation.h", "")
	if old_neg != new_neg:
		diff = "".join(difflib.unified_diff(old_neg.splitlines(True), new_neg.splitlines(True), "old/openxr_loader_negotiation.h", "new/openxr_loader_negotiation.h"))
		sync.attention.append("`openxr_loader_negotiation.h` changed, check for new interface or struct versions\n\n" + fenced(diff))

	old_refl, new_refl = old_texts.get("openxr_reflection.h", ""), new_texts.get("openxr_reflection.h", "")
	if not core_functions(new_refl):
		sync.problems.append("Found no core function lists (`XR_LIST_FUNCTIONS_XR_VERSION_*`) in the new `openxr_reflection.h`, so new core functions can't be detected")
	new_core = core_functions(new_refl) - core_functions(old_refl)
	if new_core:
		sync.attention.append("New core functions: " + ", ".join(f"`xr{n}`" for n in sorted(new_core)) + ". Trampolines were generated, check whether any need loader handling")
	new_versions = set(re.findall(r"XR_LIST_FUNCTIONS_XR_VERSION_(\d+_\d+)", new_refl)) - set(re.findall(r"XR_LIST_FUNCTIONS_XR_VERSION_(\d+_\d+)", old_refl))
	if new_versions:
		sync.attention.append("New core API version(s): " + ", ".join(v.replace("_", ".") for v in sorted(new_versions)))
	return True

def regenerate(sync, xml_path):
	old_map = read(os.path.join(ROOT, "src", "openxr_loader.map"))
	out     = run(sys.executable, os.path.join(ROOT, "tools", "gen_exports.py"), xml_path).strip().splitlines()[-1]
	new_map = read(os.path.join(ROOT, "src", "openxr_loader.map"))
	sync.auto.append(f"Regenerated `src/exports.gen.*` and the export lists ({out})")
	added   = sorted(set(export_list(new_map)) - set(export_list(old_map)))
	removed = sorted(set(export_list(old_map)) - set(export_list(new_map)))
	if added:   sync.auto.append("New exports: " + ", ".join(f"`{n}`" for n in added))
	if removed: sync.attention.append("Removed exports, which breaks the ABI for apps using them: " + ", ".join(f"`{n}`" for n in removed))

def review_source(sync, src, old, new, diffs):
	for title, paths, file_name in (("Loader source", LOADER_PATHS, "loader_source.diff"), ("Loader design doc", DOC_PATHS, "design_doc.diff")):
		# A diff over a path that moved silently reports nothing, so check each one exists at both tags
		for path in paths:
			in_old = subprocess.run(["git", "cat-file", "-e", f"release-{old}:{path}"], cwd=src, capture_output=True).returncode == 0
			in_new = subprocess.run(["git", "cat-file", "-e", f"release-{new}:{path}"], cwd=src, capture_output=True).returncode == 0
			if   in_old and not in_new: sync.problems.append(f"Upstream moved or removed `{path}`, find where it went and update `LOADER_PATHS`/`DOC_PATHS` in `tools/sync_upstream.py`")
			elif not in_old and not in_new: sync.problems.append(f"`{path}` doesn't exist upstream at either release, update `tools/sync_upstream.py`")
			elif not in_old: sync.attention.append(f"`{path}` is new upstream")

		write(os.path.join(diffs, file_name), run("git", "diff", f"release-{old}", f"release-{new}", "--", *paths, cwd=src))
		numstat = run("git", "diff", "--numstat", f"release-{old}", f"release-{new}", "--", *paths, cwd=src).split("\n")
		files   = [line.split("\t") for line in numstat if line.strip()]
		if not files:
			sync.review.append(f"- {title}: no changes")
			continue
		sync.review.append(f"- {title}: " + ", ".join(f"`{path.split('/')[-1]}` +{a}/-{d}" for a, d, path in files))
		if paths is LOADER_PATHS:
			sync.attention.append("Upstream's loader source changed, see below")

def review_changelog(sync, changelog_path, old, new, diffs):
	sections = changelog_sections(read(changelog_path), old, new)
	write(os.path.join(diffs, "changelog.md"), "\n\n".join(text for _, text in sections))
	if not sections:
		sync.problems.append(f"Found no `## OpenXR SDK X.Y.Z` sections between {old} and {new} in `{os.path.basename(changelog_path)}`, its format may have changed, check it by hand")
	sync.entries = [f"- {version}: {entry}" for version, text in sections for entry in loader_entries(text)]

def main():
	parser = argparse.ArgumentParser()
	parser.add_argument("--version",    required=True, help="Upstream SDK version to sync to, like 1.1.66")
	parser.add_argument("--work",       help="Scratch directory for downloads (default: a temp dir)")
	parser.add_argument("--report",     default=os.path.join(ROOT, "build", "sync_report.md"))
	parser.add_argument("--diffs",      default=os.path.join(ROOT, "build", "sync_diffs"), help="Where to write the full upstream diffs")
	parser.add_argument("--sdk-dir",    help="An extracted OpenXR-SDK release to use instead of downloading one")
	parser.add_argument("--source-dir", help="An OpenXR-SDK-Source clone with both release tags, instead of cloning one")
	args = parser.parse_args()

	old = header_version(read(os.path.join(HEADERS, "openxr.h")))
	new = args.version.removeprefix("release-")
	if version_key(new) <= version_key(old):
		print(f"Already at {old}, nothing to sync to {new}")
		print("changed=false\nproblems=false")
		return 0

	work = args.work or tempfile.mkdtemp(prefix="xrl_sync_")
	os.makedirs(work, exist_ok=True)
	os.makedirs(args.diffs, exist_ok=True)
	print(f"Syncing {old} -> {new} in {work}")

	sync = Sync()
	sdk  = args.sdk_dir or sync.step(f"Downloading the OpenXR-SDK release-{new} tarball", fetch_sdk, new, work)
	src  = args.source_dir or sync.step(f"Cloning OpenXR-SDK-Source at release-{old} and release-{new}", fetch_source, old, new, work)

	if sdk:
		headers   = sync.locate(sdk, SDK_HEADERS,   "openxr", is_dir=True)
		xml       = sync.locate(sdk, SDK_XML,       "xr.xml")
		changelog = sync.locate(sdk, SDK_CHANGELOG, SDK_CHANGELOG)
		updated = headers and sync.step("Updating the vendored headers", update_headers, sync, headers, new)
		# Generated trampolines must match the vendored headers, so only regenerate alongside them
		if xml and updated: sync.step("Regenerating the exports from xr.xml", regenerate, sync, xml)
		elif xml:           sync.problems.append("Didn't regenerate the exports, since the vendored headers weren't updated")
		if changelog: sync.step("Reading the changelog",                         review_changelog, sync, changelog, old, new, args.diffs)
	if src:           sync.step("Diffing upstream's loader between the releases", review_source,    sync, src, old, new, args.diffs)

	compare = f"https://github.com/KhronosGroup/OpenXR-SDK-Source/compare/release-{old}...release-{new}"
	report  = [f"# OpenXR SDK {new}\n\nAutomated sync from {old} to {new} by `tools/sync_upstream.py`. Upstream changes: [OpenXR-SDK-Source {old}...{new}]({compare}).\n"]
	if sync.problems:
		report.append("## ⚠️ Sync problems\n\nThe automatic update is incomplete. Upstream may have rearranged files this sync depends on. "
			"Everything below is what still worked.\n\n" + "\n".join(f"- {p}" for p in sync.problems) + "\n")
	report += [
		"## Auto-updated\n\n" + ("\n".join(f"- {item}" for item in sync.auto) or "Nothing.") + "\n",
		"## Needs attention\n\n" + ("\n".join(f"- {item}" for item in sync.attention) if sync.attention else "Nothing flagged.") + "\n",
		"## Loader changes to review\n\nChangelog entries mentioning the loader:\n\n" + ("\n".join(sync.entries) if sync.entries else "- none found") + "\n\n"
		"Upstream files changed:\n\n" + ("\n".join(sync.review) or "- not available") + "\n",
		"## Test results\n\nNot run.\n",
		"## Agent review prompt\n\n" + details("Prompt", fenced(AGENT_PROMPT.format(old=old, new=new), "text")),
	]
	write(args.report, "\n".join(report))
	print(f"Wrote {args.report}, {len(sync.problems)} problem(s)")
	print("changed=true")
	print(f"problems={'true' if sync.problems else 'false'}")
	return 0

if __name__ == "__main__":
	sys.exit(main())
