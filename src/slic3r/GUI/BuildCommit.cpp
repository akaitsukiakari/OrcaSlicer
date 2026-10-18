#include "BuildCommit.hpp"
#include "git_commit_hash.h"

namespace Slic3r { namespace GUI {

const char *const build_commit_hash  = GIT_COMMIT_HASH;
const char *const build_commit_label = GIT_COMMIT_HASH GIT_COMMIT_SUFFIX;
const char *const build_time         = __DATE__ " " __TIME__;

}} // namespace Slic3r::GUI
