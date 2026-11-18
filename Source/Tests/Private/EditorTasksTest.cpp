#include "TestsPCH.h"

#include "Shalltear/Editor/EditorTasks.h"

using namespace Shalltear;

namespace {

	char _firstOwner = 0;
	char _secondOwner = 0;

	class EditorTasksTest : public ::testing::Test {
	protected:
		void TearDown() override {
			EditorTasks::UnregisterOwnedBy(&_firstOwner);
			EditorTasks::UnregisterOwnedBy(&_secondOwner);
		}

		static Vector<EditorTasks::TaskView> Active() {
			Vector<EditorTasks::TaskView> tasks{GetMemoryResource(MemTag::Editor)};
			EditorTasks::GetActive(tasks);
			return tasks;
		}
	};

} // anonymous namespace

TEST_F(EditorTasksTest, TasksListInOrderUntilEnded) {
	ASSERT_TRUE(Active().empty());
	const u64 first = EditorTasks::Begin({"Downloading", {}}, &_firstOwner);
	const u64 second = EditorTasks::Begin({"Baking", []() {}}, &_firstOwner);
	EXPECT_NE(first, second);

	Vector<EditorTasks::TaskView> tasks = Active();
	ASSERT_EQ(tasks.size(), 2u);
	EXPECT_EQ(tasks[0].id, first);
	EXPECT_EQ(tasks[0].text, "Downloading");
	EXPECT_FALSE(tasks[0].fraction.has_value());
	EXPECT_FALSE(tasks[0].cancellable);
	EXPECT_EQ(tasks[1].id, second);
	EXPECT_TRUE(tasks[1].cancellable);

	EditorTasks::End(first);
	tasks = Active();
	ASSERT_EQ(tasks.size(), 1u);
	EXPECT_EQ(tasks[0].id, second);
	EditorTasks::End(second);
	EXPECT_TRUE(Active().empty());
}

TEST_F(EditorTasksTest, ReportMovesGenerationOnlyOnChange) {
	const u64 before = EditorTasks::GetGeneration();
	const u64 task = EditorTasks::Begin({"Downloading", {}}, &_firstOwner);
	const u64 begun = EditorTasks::GetGeneration();
	EXPECT_NE(begun, before);

	EditorTasks::Report(task, "Downloading (4 of 8 MB)", 0.5f);
	const u64 reported = EditorTasks::GetGeneration();
	EXPECT_NE(reported, begun);
	Vector<EditorTasks::TaskView> tasks = Active();
	ASSERT_EQ(tasks.size(), 1u);
	EXPECT_EQ(tasks[0].text, "Downloading (4 of 8 MB)");
	ASSERT_TRUE(tasks[0].fraction.has_value());
	EXPECT_FLOAT_EQ(*tasks[0].fraction, 0.5f);

	EditorTasks::Report(task, "Downloading (4 of 8 MB)", 0.5f);
	EXPECT_EQ(EditorTasks::GetGeneration(), reported);

	EditorTasks::Report(task, "Extracting");
	EXPECT_NE(EditorTasks::GetGeneration(), reported);
	EXPECT_FALSE(Active()[0].fraction.has_value());

	EditorTasks::Report(task, "Extracting", 7.0f);
	EXPECT_FLOAT_EQ(*Active()[0].fraction, 1.0f);
	EditorTasks::Report(task, "Extracting", -3.0f);
	EXPECT_FLOAT_EQ(*Active()[0].fraction, 0.0f);

	const u64 last = EditorTasks::GetGeneration();
	EditorTasks::Report(task + 100, "Nobody", 0.5f);
	EditorTasks::End(task + 100);
	EXPECT_EQ(EditorTasks::GetGeneration(), last);
	EditorTasks::End(task);
	EXPECT_NE(EditorTasks::GetGeneration(), last);
}

TEST_F(EditorTasksTest, CancelRunsCallbackAndKeepsTheTask) {
	u32 cancelled = 0;
	const u64 fixed = EditorTasks::Begin({"Hashing", {}}, &_firstOwner);
	const u64 task = EditorTasks::Begin({"Downloading", [&cancelled]() { ++cancelled; }}, &_firstOwner);

	EXPECT_FALSE(EditorTasks::Cancel(fixed));
	EXPECT_FALSE(EditorTasks::Cancel(task + 100));
	EXPECT_EQ(cancelled, 0u);
	EXPECT_TRUE(EditorTasks::Cancel(task));
	EXPECT_EQ(cancelled, 1u);
	EXPECT_EQ(Active().size(), 2u);
}

TEST_F(EditorTasksTest, CancelCallbackMayEndTheTask) {
	u64 task = 0;
	task = EditorTasks::Begin({"Downloading", [&task]() { EditorTasks::End(task); }}, &_firstOwner);
	EXPECT_TRUE(EditorTasks::Cancel(task));
	EXPECT_TRUE(Active().empty());
}

TEST_F(EditorTasksTest, OwnerSweepRemovesOnlyItsTasks) {
	EditorTasks::Begin({"First", {}}, &_firstOwner);
	const u64 kept = EditorTasks::Begin({"Second", {}}, &_secondOwner);
	EditorTasks::Begin({"Third", {}}, &_firstOwner);

	const u64 before = EditorTasks::GetGeneration();
	EXPECT_EQ(EditorTasks::UnregisterOwnedBy(&_firstOwner), 2u);
	EXPECT_NE(EditorTasks::GetGeneration(), before);
	const Vector<EditorTasks::TaskView> tasks = Active();
	ASSERT_EQ(tasks.size(), 1u);
	EXPECT_EQ(tasks[0].id, kept);

	const u64 swept = EditorTasks::GetGeneration();
	EXPECT_EQ(EditorTasks::UnregisterOwnedBy(&_firstOwner), 0u);
	EXPECT_EQ(EditorTasks::GetGeneration(), swept);
}
