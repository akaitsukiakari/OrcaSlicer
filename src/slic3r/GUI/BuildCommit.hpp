#pragma once

// Read these rather than including git_commit_hash.h, which changes with every
// commit and rebuilds everything that includes it.

namespace Slic3r { namespace GUI {

// The commit alone, safe to use in a commit URL.
extern const char *const build_commit_hash;

// The same, with "-dirty" when the build had uncommitted changes. Use this
// wherever the build is shown to a person.
extern const char *const build_commit_label;

// When BuildCommit.cpp was compiled, e.g. "Oct 18 2026 05:14:49". The file is
// rebuilt whenever the commit or its dirty state changes.
extern const char *const build_time;

}} // namespace Slic3r::GUI
