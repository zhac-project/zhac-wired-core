#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Rewrite release-manifest.json from the sibling checkouts next to this repo.
# CI builds against exactly the commits named there, so run this (and commit
# the result) whenever the firmware needs a newer sibling. Refuses a sibling
# with uncommitted changes or a commit that is not on its remote: a release
# must be rebuildable from GitHub alone.
set -euo pipefail
cd "$(dirname "$0")/.."
manifest=release-manifest.json
tmp=$(mktemp)
python3 - "$manifest" "$tmp" <<'PY'
import json, subprocess, sys, os
src, dst = sys.argv[1], sys.argv[2]
m = json.load(open(src))
for repo in m["siblings"]:
    path = os.path.join("..", repo)
    if not os.path.isdir(os.path.join(path, ".git")):
        sys.exit(f"{repo}: not checked out next to this repo")
    dirty = subprocess.run(["git", "-C", path, "status", "--porcelain", "--untracked-files=no"],
                           capture_output=True, text=True).stdout.strip()
    if dirty:
        sys.exit(f"{repo}: has uncommitted changes -- commit or stash them first")
    sha = subprocess.run(["git", "-C", path, "rev-parse", "HEAD"],
                         capture_output=True, text=True, check=True).stdout.strip()
    on_remote = subprocess.run(["git", "-C", path, "branch", "-r", "--contains", sha],
                               capture_output=True, text=True).stdout.strip()
    if not on_remote:
        sys.exit(f"{repo}: {sha[:7]} is not on any remote branch -- push it first")
    m["siblings"][repo] = sha
json.dump(m, open(dst, "w"), indent=2)
open(dst, "a").write("\n")
PY
mv "$tmp" "$manifest"
git --no-pager diff --stat -- "$manifest"
