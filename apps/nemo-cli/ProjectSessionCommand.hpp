#pragma once

#include <string>
#include <vector>

// Persistent headless project-session JSON-lines consumer. The process owns one
// ProjectSession for the lifetime of stdin, so revision conflicts and request
// deduplication are observable across retries. A request_id is scoped to this
// process/session and may be reused safely for retrying the same operation.
int commandProjectSession(const std::vector<std::string>& args);
