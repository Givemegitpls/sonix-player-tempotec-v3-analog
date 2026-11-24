#!/bin/sh
#
# Writes `<site>/versions.json` -- and, with a repository, the payload the site
# serves.
#
#   make_versions_json.sh <site-dir> <owner/repo>    # the newest tag that has a payload
#   make_versions_json.sh <site-dir> --local <name>  # this machine's build, for trying the page
#
# Only **the newest tag** is taken, and "newest" is by the tag's version, not by
# when it was published: a release for an older tag published today does not
# displace the current one, and nothing is built from a branch. A payload is
# published by .github/workflows/player-package.yml as
# `firmware-payload-<board>.tar.gz` on a release; the release keeps that complete
# archive and remains the only place the whole thing lives.
#
# The browser cannot fetch a release asset (the download redirects to
# release-assets.githubusercontent.com, which sends no
# Access-Control-Allow-Origin), so the payload is taken apart here by
# `make_site_payload.sh` into the two halves the page fetches:
#
#   payload/<tag>.player.tar.gz          payload.json, sonix_player, sonix_launch  (~2.9 MB)
#   payload/blobs/assets-<sha:12>.tar.gz assets/BLAZE + assets/V3ANALOG            (~22 MB)
#
# Needs `jq`; lists and downloads through `gh` (so: `GH_TOKEN`). When no release
# carries a payload yet, `versions` is written empty and the page says so instead of
# offering a build it does not have.
set -eu

SITE=${1:-}
REPO=${2:-}
MODE=${3:-}

if [ -z "$SITE" ]; then
	echo "usage: $0 <site-dir> <owner/repo> | <site-dir> --local <name>" >&2
	exit 2
fi

PAYLOAD_DIR="$SITE/payload"
mkdir -p "$PAYLOAD_DIR/blobs"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
ENTRIES="$TMP/entries.ndjson"
: >"$ENTRIES"

player_meta() {
	tar -xzOf "$1" payload.json
}

# One entry out of `<name>.player.tar.gz` + `<name>.assets`.
append_entry() {
	name=$1
	label=$2
	published=$3
	release=$4

	player="$PAYLOAD_DIR/$name.player.tar.gz"
	[ -f "$player" ] || { echo "make_versions_json: no $player" >&2; return 0; }
	meta=$(player_meta "$player")
	version=$(printf '%s' "$meta" | jq -r .version)
	stamp=$(printf '%s' "$meta" | jq -r .build_stamp)
	board=$(printf '%s' "$meta" | jq -r .board)
	if [ "$board" != "v3_analog" ]; then
		echo "make_versions_json: $name is a payload for $board; skipped" >&2
		return 0
	fi

	player_sha=$(sha256sum "$player" | cut -d' ' -f1)
	player_size=$(wc -c <"$player" | tr -d ' ')
	assets_url=$(cut -d' ' -f1 <"$PAYLOAD_DIR/$name.assets")
	assets_sha=$(cut -d' ' -f2 <"$PAYLOAD_DIR/$name.assets")
	assets_size=$(wc -c <"$PAYLOAD_DIR/$assets_url" | tr -d ' ')

	if [ -z "$published" ]; then published_json=null; else published_json=$(printf '"%s"' "$published"); fi

	jq -cn \
		--arg label "$label" \
		--arg version "$version" \
		--arg stamp "$stamp" \
		--argjson published "$published_json" \
		--argjson release "$release" \
		--arg player_url "payload/$name.player.tar.gz" \
		--arg player_sha "$player_sha" \
		--argjson player_size "$player_size" \
		--arg assets_url "payload/$assets_url" \
		--arg assets_sha "$assets_sha" \
		--argjson assets_size "$assets_size" \
		'{label: $label, version: $version, stamp: $stamp, published: $published, release: $release,
		  player: {url: $player_url, size: $player_size, sha256: $player_sha},
		  assets: {url: $assets_url, size: $assets_size, sha256: $assets_sha}}' >>"$ENTRIES"
}

if [ "$REPO" = "--local" ]; then
	# A local stage: the build in this tree, named by the caller.
	[ -n "$MODE" ] || { echo "make_versions_json: --local needs a name" >&2; exit 2; }
	append_entry "$MODE" "$MODE" '' false
else
	[ -n "$REPO" ] || { echo "make_versions_json: no repository and no --local" >&2; exit 2; }
	if gh api "repos/$REPO/releases?per_page=100" >"$TMP/releases.json" 2>/dev/null; then
		# The greatest tag version that carries a payload. `sort -V` is the whole
		# point: a release is identified by its tag, not by when it was published.
		tag=$(jq -r '
			.[]
			| select(any(.assets[]?; .name | test("^firmware-payload-.*[.]tar[.]gz$")))
			| .tag_name
		' "$TMP/releases.json" | sort -V | tail -n 1)

		if [ -n "$tag" ]; then
			published=$(jq -r --arg tag "$tag" '.[] | select(.tag_name == $tag) | .published_at' "$TMP/releases.json")
			mkdir -p "$TMP/rel"
			if gh release download "$tag" --pattern 'firmware-payload-*.tar.gz*' --dir "$TMP/rel" --clobber 2>/dev/null; then
				for payload in "$TMP/rel"/firmware-payload-*.tar.gz; do
					[ -f "$payload" ] || continue
					board=$(tar -xzOf "$payload" payload.json | jq -r .board)
					[ "$board" = "v3_analog" ] || continue
					"$(dirname "$0")/make_site_payload.sh" "$payload" "$SITE" "$tag" >/dev/null
					append_entry "$tag" "$tag" "$published" true
					break
				done
			else
				echo "make_versions_json: $tag has no downloadable payload" >&2
			fi
		else
			echo "make_versions_json: no release carries a firmware payload yet" >&2
		fi
	fi
fi

jq -s '{format: 1, versions: .}' "$ENTRIES" >"$SITE/versions.json"
if [ "$(jq '.versions | length' "$SITE/versions.json")" = "0" ]; then
	echo "make_versions_json: nothing to offer -- versions.json is empty"
else
	jq -r '.versions[] | "    \(.label)  \(.version)  player \(.player.size) B + assets \(.assets.size) B"' \
		"$SITE/versions.json"
fi
