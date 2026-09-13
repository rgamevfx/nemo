#include "nemo/core/SharedContainers.hpp"

#include <algorithm>
#include <numeric>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace nemo {
namespace {

using Sequence = CowVector<int, 8>;

std::vector<int> materialize(const Sequence& sequence) {
    return std::vector<int>(sequence.begin(), sequence.end());
}

TEST(CowVectorTest, CopiesShareStorageUntilOneVersionMutates) {
    Sequence original;
    for (int value = 0; value < 20; ++value)
        original.push_back(value);

    Sequence snapshot = original;
    EXPECT_EQ(materialize(snapshot), materialize(original));

    original.mutableAt(0) = 100;
    original.mutableAt(19) = 119;
    EXPECT_EQ(original[0], 100);
    EXPECT_EQ(original[19], 119);
    // The retained version still reads the authored values it captured.
    EXPECT_EQ(snapshot[0], 0);
    EXPECT_EQ(snapshot[19], 19);

    snapshot.push_back(20);
    EXPECT_EQ(snapshot.size(), 21u);
    EXPECT_EQ(original.size(), 20u);
    EXPECT_EQ(original.back(), 119);
}

TEST(CowVectorTest, IteratorEqualityAndTraversalSurviveTemporaryViews) {
    Sequence sequence;
    for (int value = 0; value < 40; ++value)
        sequence.push_back(value);
    const Sequence view = sequence;  // a second view over the same storage

    const auto found = std::find_if(view.begin(), view.end(), [](int value) { return value == 33; });
    EXPECT_NE(found, view.end());
    EXPECT_EQ(*found, 33);
    // A second view of the same unchanged storage produces equal iterators.
    EXPECT_EQ(found, sequence.begin() + 33);
    EXPECT_EQ(std::accumulate(sequence.begin(), sequence.end(), 0), 780);
}

TEST(CowVectorTest, InsertAndEraseKeepOrderAcrossChunkBoundaries) {
    Sequence sequence;
    for (int value = 0; value < 20; ++value)
        sequence.push_back(value);

    sequence.insert(9, 900);
    EXPECT_EQ(sequence.size(), 21u);
    std::vector<int> expected;
    for (int value = 0; value < 20; ++value) {
        if (value == 9)
            expected.push_back(900);
        expected.push_back(value);
    }
    EXPECT_EQ(materialize(sequence), expected);

    sequence.erase(9);
    EXPECT_EQ(sequence.size(), 20u);
    expected.clear();
    for (int value = 0; value < 20; ++value)
        expected.push_back(value);
    EXPECT_EQ(materialize(sequence), expected);
}

TEST(CowVectorTest, ErasingEveryRecordOneByOneKeepsTheSequenceAddressable) {
    Sequence sequence;
    for (int value = 0; value < 101; ++value)
        sequence.push_back(value);

    for (int remaining = 101; remaining > 0; --remaining) {
        ASSERT_EQ(sequence.size(), static_cast<std::size_t>(remaining));
        sequence.erase(static_cast<std::size_t>(remaining) / 2);
    }
    EXPECT_TRUE(sequence.empty());
    EXPECT_EQ(sequence.begin(), sequence.end());
}

TEST(CowVectorTest, EraseIfRemovesOnlyAcceptedRecordsAndPreservesOrder) {
    Sequence sequence;
    for (int value = 0; value < 50; ++value)
        sequence.push_back(value);
    Sequence snapshot = sequence;

    sequence.eraseIf([](int value) { return value % 5 == 0; });
    std::vector<int> expected;
    for (int value = 0; value < 50; ++value)
        if (value % 5 != 0)
            expected.push_back(value);
    EXPECT_EQ(materialize(sequence), expected);
    EXPECT_EQ(snapshot.size(), 50u);
}

TEST(CowVectorTest, AssignReplacesContentsAndClearEmptiesThem) {
    Sequence sequence;
    for (int value = 0; value < 5; ++value)
        sequence.push_back(value);
    Sequence snapshot = sequence;

    std::vector<int> replacement(70);
    std::iota(replacement.begin(), replacement.end(), 1000);
    sequence.assign(replacement);
    EXPECT_EQ(sequence.size(), 70u);
    EXPECT_EQ(sequence.front(), 1000);
    EXPECT_EQ(sequence.back(), 1069);
    EXPECT_EQ(snapshot.size(), 5u);

    sequence.clear();
    EXPECT_TRUE(sequence.empty());
}

TEST(CowVectorTest, MutatingThroughAStoredPointerLeavesOtherVersionsUntouched) {
    struct Record {
        std::string name;
        int value{};
    };
    CowVector<Record, 4> original;
    for (int index = 0; index < 10; ++index)
        original.push_back(Record{"name" + std::to_string(index), index});
    CowVector<Record, 4> snapshot = original;

    Record& record = original.mutableAt(5);
    record.value = 500;
    original.mutableAt(6).name = "changed";
    EXPECT_EQ(snapshot[5].value, 5);
    EXPECT_EQ(snapshot[6].name, "name6");
    EXPECT_EQ(original[5].value, 500);
}

TEST(CowVectorTest, FindAndIndexOfLocateRecordsByValue) {
    Sequence sequence;
    for (int value = 0; value < 40; ++value)
        sequence.push_back(value * 2);

    EXPECT_EQ(sequence.indexOf([](int value) { return value == 40; }), 20u);
    ASSERT_NE(sequence.find([](int value) { return value == 40; }), nullptr);
    EXPECT_EQ(*sequence.find([](int value) { return value == 40; }), 40);
    EXPECT_EQ(sequence.find([](int value) { return value == 41; }), nullptr);
    EXPECT_EQ(sequence.indexOf([](int value) { return value == 41; }), sequence.size());
}

TEST(CowMapTest, CopiesShareStorageUntilOneVersionMutates) {
    using IntMap = CowMap<std::string, int>;
    IntMap original;
    original["alpha"] = 1;
    original["beta"] = 2;

    IntMap snapshot = original;
    EXPECT_EQ(snapshot.size(), 2u);
    EXPECT_TRUE(snapshot.contains("alpha"));
    EXPECT_EQ(snapshot.at("beta"), 2);

    original["alpha"] = 10;
    original["gamma"] = 3;
    EXPECT_EQ(original.at("alpha"), 10);
    EXPECT_EQ(snapshot.at("alpha"), 1);
    EXPECT_FALSE(snapshot.contains("gamma"));
    EXPECT_EQ(snapshot, IntMap(snapshot));
}

TEST(CowMapTest, OrderedIterationAndBoundsMatchTheOrderedMapContract) {
    CowMap<std::string, int> map;
    map["delta"] = 4;
    map["alpha"] = 1;
    map["charlie"] = 3;
    map["bravo"] = 2;

    std::vector<std::string> keys;
    for (const auto& [key, value] : map) {
        static_cast<void>(value);
        keys.push_back(key);
    }
    EXPECT_EQ(keys, (std::vector<std::string>{"alpha", "bravo", "charlie", "delta"}));
    EXPECT_EQ(map.upper_bound("bravo")->first, "charlie");
    EXPECT_EQ(map.lower_bound("bravo")->first, "bravo");

    EXPECT_EQ(map.erase("bravo"), 1u);
    EXPECT_EQ(map.erase("bravo"), 0u);
    EXPECT_EQ(map.size(), 3u);
    EXPECT_EQ(map.find("bravo"), map.end());
}

}  // namespace
}  // namespace nemo
