// The relay's command codes against the node's own commands.h: they cannot drift.
// node_protocol.h is read first and its values kept under other names, because
// commands.h defines the same names as macros.
#include "node_protocol.h"

#define RELAY_CODES(X)                                                                                        \
    X(READ_FREQUENCY) X(TEST_RX_REGISTER) X(READ_LAP_PASS_STATS) X(READ_LAP_EXTREMUMS) X(READ_REVISION_CODE) \
    X(READ_ENTER_AT_LEVEL) X(READ_EXIT_AT_LEVEL) X(READ_TIME_MILLIS) X(READ_MULTINODE_COUNT)                 \
    X(READ_CURNODE_INDEX) X(READ_FW_VERSION) X(READ_FW_PROCTYPE) X(WRITE_FREQUENCY) X(WRITE_ENTER_AT_LEVEL)  \
    X(WRITE_EXIT_AT_LEVEL) X(WRITE_CURNODE_INDEX) X(LAPSTATS_FLAG_CROSSING)

namespace kept {
#define KEEP(name) constexpr int name##_ = relay::name;
RELAY_CODES(KEEP)
#undef KEEP
}  // namespace kept

#include "RssiNode.h"
#include "commands.h"

#define CHECK_CODE(name) static_assert(kept::name##_ == name, #name " differs from src/node/commands.h");
RELAY_CODES(CHECK_CODE)
static_assert(relay::FW_TEXT_BLOCK_SIZE == TEXT_BLOCK_SIZE, "text block size differs");

int relayConstantsChecked() { return 1; }
