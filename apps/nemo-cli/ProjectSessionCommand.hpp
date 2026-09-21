#pragma once

#include <memory>
#include <string>
#include <vector>

namespace nemo {
class NodeContributions;
}  // namespace nemo

// Persistent headless project-session JSON-lines consumer. The process owns one
// ProjectSession for the lifetime of stdin, so revision conflicts and request
// deduplication are observable across retries. A request_id is scoped to this
// process/session and may be reused safely for retrying the same operation.
//
// `contributions` is the composed node inventory assembled once at the
// process composition root: the session is constructed with it, and every
// project read resolves node types and missing dependencies against its
// catalog. The command never falls back to the built-in inventory on its own.
int commandProjectSession(const std::vector<std::string>& args,
                          std::shared_ptr<const nemo::NodeContributions> contributions);
