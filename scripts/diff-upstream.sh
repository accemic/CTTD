#!/usr/bin/env bash
# Diff this NexRv fork against the upstream tg-nexus-trace refcode/c/.
#
# Clones or updates ./upstream/tg-nexus-trace, checks out the latest upstream
# revision, and launches `git difftool --dir-diff` between the upstream
# refcode/c/ directory and this fork's src/.
#
# Environment overrides:
#   NEXRV_UPSTREAM_URL  - upstream git URL
#                         (default: https://github.com/riscv-non-isa/tg-nexus-trace.git)
#   NEXRV_UPSTREAM_REF  - upstream revision to compare against
#                         (default: origin/master)
#
# Requires `git difftool --dir-diff` to be configured (e.g. `git config --global
# diff.tool meld`). The upstream clone lives under ./upstream/ which is
# gitignored.

set -euo pipefail

# Resolve the repo root from the script path (handles symlinks and being
# invoked from outside the repo).
SCRIPT_PATH="$(readlink -f "$0" 2>/dev/null || python3 -c 'import os,sys;print(os.path.realpath(sys.argv[1]))' "$0")"
REPO_ROOT="$(cd "$(dirname "$SCRIPT_PATH")/.." && pwd)"

UPSTREAM_URL="${NEXRV_UPSTREAM_URL:-https://github.com/riscv-non-isa/tg-nexus-trace.git}"
UPSTREAM_DIR="${REPO_ROOT}/upstream/tg-nexus-trace"
# Default to whatever the upstream considers its default branch (main, master,
# etc.). origin/HEAD is a symbolic ref set by `git clone`.
UPSTREAM_REF="${NEXRV_UPSTREAM_REF:-origin/HEAD}"

log() { echo "[diff-upstream] $*"; }

# 0. Preflight: ensure the user has a git difftool configured. `--dir-diff`
#    won't fall back to a prompt sensibly — it just errors out — so it's
#    nicer to catch this up front.
if [ -z "$(git config --get diff.tool || true)" ] && \
   [ -z "${GIT_DIFFTOOL_PROMPT:-}" ] && \
   [ -z "${NEXRV_DIFFTOOL_OK:-}" ]; then
  cat >&2 <<'EOF'
[diff-upstream] ERROR: no diff.tool configured for git.
  Configure one, e.g.:
      git config --global diff.tool meld          # GUI side-by-side
      git config --global diff.tool vimdiff       # terminal
      git config --global diff.tool kdiff3        # GUI 3-way
  Then re-run. (Set NEXRV_DIFFTOOL_OK=1 to bypass this check.)
EOF
  exit 2
fi

# 1. Clone or update the upstream checkout.
if [ -d "$UPSTREAM_DIR/.git" ]; then
  log "fetching $UPSTREAM_URL ..."
  git -C "$UPSTREAM_DIR" fetch --quiet origin
else
  log "cloning $UPSTREAM_URL into ${UPSTREAM_DIR#$REPO_ROOT/} ..."
  mkdir -p "$(dirname "$UPSTREAM_DIR")"
  git clone --quiet "$UPSTREAM_URL" "$UPSTREAM_DIR"
fi

# 2. Move the upstream working tree to the requested revision. Checking out a
#    remote-tracking ref (e.g. origin/HEAD) lands in detached HEAD naturally,
#    so we never disturb any branch the user may have checked out there.
git -C "$UPSTREAM_DIR" -c advice.detachedHead=false checkout --quiet "$UPSTREAM_REF"

UPSTREAM_SHA="$(git -C "$UPSTREAM_DIR" rev-parse --short HEAD)"
UPSTREAM_DATE="$(git -C "$UPSTREAM_DIR" log -1 --format=%ai HEAD)"
log "upstream @ $UPSTREAM_SHA  ($UPSTREAM_DATE)  ref=$UPSTREAM_REF"

UPSTREAM_SRC="$UPSTREAM_DIR/refcode/c"
LOCAL_SRC="$REPO_ROOT/src"
if [ ! -d "$UPSTREAM_SRC" ]; then
  echo "[diff-upstream] ERROR: $UPSTREAM_SRC not found in upstream checkout." >&2
  exit 1
fi

# 3. Open the directory-diff session.
#    `git difftool --dir-diff` only works between two git trees in the same
#    repo (incompatible with --no-index), so we invoke the user's configured
#    tool directly on the two paths. Most GUI diff tools (meld, kdiff3, p4merge,
#    diffmerge, bcompare, ...) accept two directory arguments natively.
#    Argument order is OLD then NEW (upstream first, fork second), so the diff
#    reads naturally as "what this fork added/changed on top of upstream".
TOOL="$(git config --get diff.tool)"
if ! command -v "$TOOL" >/dev/null 2>&1; then
  echo "[diff-upstream] ERROR: configured diff.tool '$TOOL' not found on PATH." >&2
  exit 3
fi
log "launching: $TOOL ${UPSTREAM_SRC#$REPO_ROOT/} src"
exec "$TOOL" "$UPSTREAM_SRC" "$LOCAL_SRC"
