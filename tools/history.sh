#!/bin/sh
#
# history.sh -- read the opencode session archive.
#
# The archive is the only record of how parts of this port were worked out:
# the two-day LDAC debugging, the kernel.bin disassembly, the Bluetooth A2DP
# sink research, the UI measurements. It lives in
#
#   ~/.local/share/opencode/opencode.db        the transcripts
#   ~/.local/share/opencode/tool-output/       full outputs the tool truncated
#   ~/.local/share/opencode/snapshot/          git object stores of file states
#
# and the current tables are the V2 ones (session_v2 + session_message); the old
# session/part tables stop at 2026-09-15. Message payloads are JSON, with a
# content[] array whose entries are text, reasoning or tool -- and a tool entry
# carries both the call (state.input) and its output (state.content), which is
# where the measurements and register dumps ended up.
#
# Usage:
#   history.sh sessions [keyword]        list sessions (all, or those mentioning keyword)
#   history.sh grep <keyword> [chars]    matching sessions with a snippet of the hit
#   history.sh text <session-id> [kw]    the text the agent and the user wrote
#   history.sh tools <session-id> [kw]   tool calls with their outputs
#   history.sh files <session-id>        files the session touched (paths only)
#   history.sh snaps                     snapshot stores, with unreferenced commit counts
#
# Keywords are SQL LIKE patterns: % and _ work, matching is case-insensitive for
# ASCII. Set CHARS to widen the excerpts (default 400), DB to use another archive.
#
set -eu

DB="${DB:-$HOME/.local/share/opencode/opencode.db}"
SNAP="${SNAP:-$HOME/.local/share/opencode/snapshot}"
CHARS="${CHARS:-400}"

command -v sqlite3 >/dev/null 2>&1 || {
	echo "history.sh: sqlite3 is not installed" >&2
	exit 1
}
[ -f "$DB" ] || {
	echo "history.sh: no archive at $DB" >&2
	exit 1
}

# Read-only, so the archive can be queried while opencode has it open.
q() { sqlite3 -readonly "file:$DB?mode=ro" "$@"; }

cmd="${1:-help}"
shift 2>/dev/null || true

case "$cmd" in
sessions)
	kw="${1:-}"
	if [ -z "$kw" ]; then
		q "SELECT v.id, date(max(m.time_created)/1000,'unixepoch') d, count(*), substr(v.title,1,60)
		     FROM session_message m JOIN session_v2 v ON v.id = m.session_id
		    GROUP BY v.id ORDER BY d DESC, v.id;"
	else
		q "SELECT v.id, date(max(m.time_created)/1000,'unixepoch') d, count(*), substr(v.title,1,60)
		     FROM session_message m JOIN session_v2 v ON v.id = m.session_id
		    WHERE m.data LIKE '%$kw%'
		    GROUP BY v.id ORDER BY d DESC;"
	fi
	;;
grep)
	kw="${1:?usage: history.sh grep <keyword> [chars]}"
	chars="${2:-$CHARS}"
	q "SELECT v.id || '  ' || date(m.time_created/1000,'unixepoch') || '  ' ||
	          substr(replace(json_extract(j.value,'\$.text'), char(10), ' '), 1, $chars)
	     FROM session_message m, json_each(m.data,'\$.content') j
	     JOIN session_v2 v ON v.id = m.session_id
	    WHERE json_extract(j.value,'\$.type') = 'text'
	      AND json_extract(j.value,'\$.text') LIKE '%$kw%'
	    ORDER BY m.time_created;"
	;;
text)
	sid="${1:?usage: history.sh text <session-id> [keyword]}"
	kw="${2:-%}"
	q "SELECT date(m.time_created/1000,'unixepoch','localtime') || '  ' ||
	          substr(replace(json_extract(j.value,'\$.text'), char(10), char(10) || '    '), 1, $CHARS)
	     FROM session_message m, json_each(m.data,'\$.content') j
	    WHERE m.session_id = '$sid'
	      AND json_extract(j.value,'\$.type') = 'text'
	      AND json_extract(j.value,'\$.text') LIKE '%$kw%'
	    ORDER BY m.time_created;"
	;;
tools)
	sid="${1:?usage: history.sh tools <session-id> [keyword]}"
	kw="${2:-%}"
	q "SELECT '--- ' || json_extract(j.value,'\$.name') || ' ' ||
	          substr(replace(coalesce(json_extract(j.value,'\$.state.input'),''), char(10), ' '), 1, 200) || char(10) ||
	          substr(replace(coalesce(json_extract(j.value,'\$.state.content[0].text'),''), char(10), ' '), 1, $CHARS)
	     FROM session_message m, json_each(m.data,'\$.content') j
	    WHERE m.session_id = '$sid'
	      AND json_extract(j.value,'\$.type') = 'tool'
	      AND coalesce(json_extract(j.value,'\$.state.content[0].text'),'') LIKE '%$kw%'
	    ORDER BY m.time_created;"
	;;
files)
	sid="${1:?usage: history.sh files <session-id>}"
	q "SELECT DISTINCT coalesce(json_extract(j.value,'\$.state.input.path'),
	                            json_extract(j.value,'\$.state.input.filePath'),
	                            json_extract(j.value,'\$.state.input.file_path'))
	     FROM session_message m, json_each(m.data,'\$.content') j
	    WHERE m.session_id = '$sid' AND json_extract(j.value,'\$.type') = 'tool'
	      AND coalesce(json_extract(j.value,'\$.state.input.path'),
	                   json_extract(j.value,'\$.state.input.filePath'),
	                   json_extract(j.value,'\$.state.input.file_path')) IS NOT NULL
	    ORDER BY 1;"
	;;
snaps)
	[ -d "$SNAP" ] || {
		echo "history.sh: no snapshot store at $SNAP" >&2
		exit 1
	}
	for outer in "$SNAP"/*/; do
		for repo in "$outer"*/; do
			[ -d "$repo/objects" ] || continue
			n=$(git --git-dir="${repo%/}" cat-file --batch-all-objects --batch-check 2>/dev/null | wc -l)
			echo "${repo%/}  objects=$n"
		done
	done
	echo
	echo "Commits here are unreferenced (no refs), so walk them with:"
	echo "  git --git-dir=<repo> cat-file --batch-all-objects --batch-check | awk '\$2==\"commit\"{print \$1}'"
	;;
*)
	sed -n '3,30p' "$0" | sed 's/^# \{0,1\}//'
	;;
esac
