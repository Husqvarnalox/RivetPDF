#include "RivetTest.h"

#include "editor/Command.hpp"
#include "editor/CommandStack.hpp"

#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>

using rivet::editor::Command;
using rivet::editor::CommandStack;

namespace {

// Counters live outside the command object: the stack owns (and destroys)
// commands it holds, and a command whose execute() failed is destroyed by
// execute() itself. Tests therefore hold shared_ptr copies of the stats so
// call counts stay verifiable after the command object is gone.
struct CommandStats {
    int applied = 0;
    int undone = 0;
};

struct CountingCommand final : Command {
    explicit CountingCommand(std::string_view label, bool shouldSucceed = true)
        : name_(label), succeed_(shouldSucceed), stats_(std::make_shared<CommandStats>()) {}

    std::string_view name() const override { return name_; }
    bool execute() override {
        ++stats_->applied;
        return succeed_;
    }
    bool undo() override {
        ++stats_->undone;
        return true;
    }

    std::string_view name_;
    bool succeed_;
    std::shared_ptr<CommandStats> stats_;
};

} // namespace

RIVET_TEST(executePushesAndUndoRedo) {
    CommandStack stack;
    auto command = std::make_unique<CountingCommand>("op");
    auto stats = command->stats_;

    CHECK(stack.execute(std::move(command)));
    CHECK_EQ(stack.depth(), std::size_t{1});
    CHECK(stack.canUndo());
    CHECK(!stack.canRedo());
    CHECK_EQ(stats->applied, 1);
    CHECK_EQ(stats->undone, 0);

    CHECK(stack.undo());
    CHECK_EQ(stats->applied, 1);
    CHECK_EQ(stats->undone, 1);
    CHECK(stack.canRedo());
    CHECK(!stack.canUndo());
    CHECK_EQ(stack.depth(), std::size_t{0});

    CHECK(stack.redo()); // default redo() delegates to execute()
    CHECK_EQ(stats->applied, 2);
    CHECK_EQ(stats->undone, 1);
    CHECK_EQ(stack.depth(), std::size_t{1});
    CHECK(stack.canUndo());
    CHECK(!stack.canRedo());
}

RIVET_TEST(failedCommandNotPushed) {
    CommandStack stack;
    auto command = std::make_unique<CountingCommand>("broken", /*shouldSucceed=*/false);
    auto stats = command->stats_;

    CHECK(!stack.execute(std::move(command)));
    CHECK_EQ(stack.depth(), std::size_t{0});
    CHECK(!stack.canUndo());
    CHECK(!stack.canRedo());
    // The command ran (and failed) exactly once.
    CHECK_EQ(stats->applied, 1);
    CHECK_EQ(stats->undone, 0);

    // Undo/redo on an empty stack are no-ops.
    CHECK(!stack.undo());
    CHECK(!stack.redo());
}

RIVET_TEST(executeClearsRedoStack) {
    CommandStack stack;
    auto a = std::make_unique<CountingCommand>("a");
    auto b = std::make_unique<CountingCommand>("b");
    auto statsA = a->stats_;
    auto statsB = b->stats_;

    CHECK(stack.execute(std::move(a)));
    CHECK(stack.undo());
    CHECK(stack.canRedo());

    // A new execute() must discard the redo branch.
    CHECK(stack.execute(std::move(b)));
    CHECK(!stack.canRedo());
    CHECK_EQ(stack.depth(), std::size_t{1});

    // Undoing now rolls back B, not the abandoned A: B is undone once, A stays
    // at its single pre-abandonment undo and is never re-applied.
    CHECK(stack.undo());
    CHECK_EQ(statsB->undone, 1);
    CHECK_EQ(statsA->undone, 1);
    CHECK_EQ(statsB->applied, 1);
    CHECK_EQ(statsA->applied, 1);
}

RIVET_TEST(maxDepthEvictsOldest) {
    CommandStack stack;
    stack.setMaxDepth(2);

    auto first = std::make_unique<CountingCommand>("first");
    auto second = std::make_unique<CountingCommand>("second");
    auto third = std::make_unique<CountingCommand>("third");
    auto statsFirst = first->stats_;
    auto statsSecond = second->stats_;
    auto statsThird = third->stats_;

    CHECK(stack.execute(std::move(first)));
    CHECK(stack.execute(std::move(second)));
    CHECK(stack.execute(std::move(third)));
    CHECK_EQ(stack.depth(), std::size_t{2});

    // The oldest entry ("first") was evicted: undoing twice succeeds, the
    // third undo fails.
    CHECK(stack.undo());
    CHECK_EQ(statsThird->undone, 1);
    CHECK(stack.undo());
    CHECK_EQ(statsSecond->undone, 1);
    CHECK(!stack.undo());
    CHECK_EQ(statsFirst->undone, 0); // never reached: it was dropped

    // Redo works for exactly the two survivors.
    CHECK(stack.redo());
    CHECK(stack.redo());
    CHECK(!stack.redo());
    CHECK_EQ(statsThird->applied, 2);
    CHECK_EQ(statsSecond->applied, 2);
    CHECK_EQ(statsFirst->applied, 1);
    CHECK_EQ(stack.depth(), std::size_t{2});
}

RIVET_TEST(shrinkingMaxDepthEvictsImmediately) {
    CommandStack stack(4);
    for (int i = 0; i < 4; ++i) {
        CHECK(stack.execute(std::make_unique<CountingCommand>("op")));
    }
    CHECK_EQ(stack.depth(), std::size_t{4});

    stack.setMaxDepth(1);
    CHECK_EQ(stack.depth(), std::size_t{1});
    CHECK(stack.canUndo());
    CHECK(stack.undo());
    CHECK(!stack.canUndo());
}

RIVET_TEST(clearResetsState) {
    CommandStack stack;
    auto command = std::make_unique<CountingCommand>("op");
    auto stats = command->stats_;

    CHECK(stack.execute(std::move(command)));
    CHECK(stack.undo());

    stack.clear();
    CHECK_EQ(stack.depth(), std::size_t{0});
    CHECK(!stack.canUndo());
    CHECK(!stack.canRedo());
    // clear() drops without undoing or re-running anything.
    CHECK_EQ(stats->applied, 1);
    CHECK_EQ(stats->undone, 1);
}

RIVET_TEST(onChangedCallbackFires) {
    CommandStack stack;
    int changes = 0;
    stack.setOnChanged([&changes] { ++changes; });

    auto command = std::make_unique<CountingCommand>("op");
    CHECK(stack.execute(std::move(command)));
    CHECK_EQ(changes, 1);

    CHECK(stack.undo());
    CHECK_EQ(changes, 2);

    CHECK(stack.redo());
    CHECK_EQ(changes, 3);

    stack.clear();
    CHECK_EQ(changes, 4);

    // A failed execute() changes nothing and must not notify.
    CHECK(!stack.execute(nullptr));
    CHECK(!stack.execute(std::make_unique<CountingCommand>("broken", false)));
    CHECK_EQ(changes, 4);
}

RIVET_TEST(stateIdsTrackHistoryPositions) {
    CommandStack stack(2);
    CHECK_EQ(stack.stateId(), std::uint64_t{0});
    CHECK(stack.execute(std::make_unique<CountingCommand>("a")));
    const std::uint64_t a = stack.stateId();
    CHECK(a != 0);
    CHECK(stack.undo());
    CHECK_EQ(stack.stateId(), std::uint64_t{0});
    CHECK(stack.redo());
    CHECK_EQ(stack.stateId(), a);
    CHECK(stack.undo());
    // A new command after an undo never reuses an old id.
    CHECK(stack.execute(std::make_unique<CountingCommand>("b")));
    const std::uint64_t b = stack.stateId();
    CHECK(b != a && b != 0);
    // Failed commands leave the state alone.
    CHECK(!stack.execute(std::make_unique<CountingCommand>("fail", false)));
    CHECK_EQ(stack.stateId(), b);
    // Eviction (depth 2): undoing everything reaches the state after the
    // evicted command, not the initial one.
    CHECK(stack.execute(std::make_unique<CountingCommand>("c")));
    CHECK(stack.execute(std::make_unique<CountingCommand>("d")));
    const std::uint64_t d = stack.stateId();
    while (stack.undo()) {
    }
    CHECK_EQ(stack.stateId(), b);
    // clear() keeps the current state.
    while (stack.redo()) {
    }
    stack.clear();
    CHECK_EQ(stack.stateId(), d);
}
