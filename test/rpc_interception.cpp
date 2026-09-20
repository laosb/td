#include "td/telegram/net/RpcInterception.h"
#include "td/utils/ChainScheduler.h"
#include "td/utils/tests.h"

#include <memory>

TEST(RpcInterception, ConfigurationIsExplicitAndBounded) {
  auto configuration = td::RpcInterceptionConfiguration::create({-2, 42, -2}, 30).move_as_ok();
  ASSERT_TRUE(configuration->contains(-2));
  ASSERT_TRUE(configuration->contains(42));
  ASSERT_TRUE(!configuration->contains(43));
  ASSERT_EQ(2u, configuration->constructors.size());
  ASSERT_TRUE(td::RpcInterceptionConfiguration::create({}, 0).is_error());
  ASSERT_TRUE(td::RpcInterceptionConfiguration::create({}, 121).is_error());
  ASSERT_TRUE(td::RpcInterceptionConfiguration::create(td::vector<td::int32>(65, 1), 60).is_error());
  ASSERT_TRUE(!td::RpcInterceptionConfiguration::create({}, 60).move_as_ok()->contains(42));
}

TEST(RpcInterception, ContinuationsAreBoundedAndConsumedOnce) {
  td::RpcInterceptionPending<std::unique_ptr<int>> pending;
  for (int i = 1; i <= 64; ++i) {
    ASSERT_TRUE(pending.insert(i, 20, std::make_unique<int>(i)));
  }
  ASSERT_TRUE(!pending.can_insert());
  ASSERT_TRUE(!pending.insert(65, 20, std::make_unique<int>(65)));
  ASSERT_EQ(37, *pending.take(37).move_as_ok());
  ASSERT_TRUE(pending.take(37).is_error());
  ASSERT_TRUE(pending.take(99).is_error());
  ASSERT_TRUE(pending.can_insert());
  ASSERT_TRUE(pending.insert(65, 20, std::make_unique<int>(65)));
}

TEST(RpcInterception, CreationBudgetSurvivesReconfiguration) {
  auto budget = std::make_shared<td::RpcInterceptionBudget>();
  auto old_configuration = td::RpcInterceptionConfiguration::create({1}, 1, budget).move_as_ok();
  auto new_configuration = td::RpcInterceptionConfiguration::create({2}, 1, budget).move_as_ok();
  td::vector<std::unique_ptr<td::RpcInterceptionBudget::Reservation>> reservations;
  for (int i = 0; i < 64; ++i) {
    auto reservation = old_configuration->budget->reserve();
    ASSERT_TRUE(reservation != nullptr);
    reservations.push_back(std::move(reservation));
  }
  ASSERT_TRUE(new_configuration->budget->reserve() == nullptr);
  old_configuration.reset();
  ASSERT_TRUE(new_configuration->budget->reserve() == nullptr);
  reservations.pop_back();
  auto admitted = new_configuration->budget->reserve();
  ASSERT_TRUE(admitted != nullptr);
  ASSERT_TRUE(new_configuration->budget->reserve() == nullptr);
  reservations.clear();
  admitted.reset();
  ASSERT_TRUE(new_configuration->budget->reserve() != nullptr);
}

TEST(RpcInterception, QueuedExpiryReleasesBarrierBehindStalledPredecessor) {
  using TaskId = td::ChainScheduler<int>::TaskId;
  td::ChainScheduler<int> scheduler;
  td::RpcInterceptionPending<TaskId, TaskId> queued;
  auto predecessor = scheduler.create_task({td::uint64{1}}, 1);
  ASSERT_EQ(predecessor, scheduler.start_next_task().unwrap().task_id);
  auto protected_request = scheduler.create_task({td::uint64{1}}, 2, true);
  ASSERT_TRUE(queued.insert(protected_request, 10, protected_request));
  auto successor = scheduler.create_task({td::uint64{1}}, 3);
  auto unrelated = scheduler.create_task({td::uint64{2}}, 4);
  ASSERT_EQ(unrelated, scheduler.start_next_task().unwrap().task_id);
  ASSERT_TRUE(!scheduler.start_next_task());

  td::vector<TaskId> expired;
  queued.remove_if([](auto &entry) { return entry.deadline <= 10; },
                   [&](TaskId id, TaskId) { expired.push_back(id); });
  ASSERT_EQ(td::vector<TaskId>({protected_request}), expired);
  for (auto id : expired) {
    scheduler.pause_task(id);
  }
  for (auto id : expired) {
    scheduler.finish_task(id);
  }
  // Expiry completes only the unsent protected query. Ordinary pipelining
  // resumes while the original network request is still deliberately stalled.
  ASSERT_EQ(1, *scheduler.get_task_extra(predecessor));
  auto next = scheduler.start_next_task().unwrap();
  ASSERT_EQ(successor, next.task_id);
  ASSERT_EQ(td::vector<TaskId>({predecessor}), next.parents);
  ASSERT_TRUE(!scheduler.start_next_task());
  ASSERT_TRUE(queued.empty());
  scheduler.finish_task(successor);
  scheduler.finish_task(predecessor);
  scheduler.finish_task(unrelated);
}

TEST(RpcInterception, ExpiryCancellationAndConfigurationChangeReleaseOwnership) {
  td::RpcInterceptionPending<std::unique_ptr<int>> pending;
  pending.insert(1, 10, std::make_unique<int>(1));
  pending.insert(2, 11, std::make_unique<int>(2));
  pending.insert(3, 12, std::make_unique<int>(3));
  td::vector<td::int64> completed;
  auto finish = [&](td::int64 id, std::unique_ptr<int> value) {
    ASSERT_EQ(id, *value);
    completed.push_back(id);
  };
  // Expiry is inclusive; a canceled query is removed before consuming any reply.
  pending.remove_if([](auto &entry) { return entry.deadline <= 10 || *entry.value == 3; }, finish);
  ASSERT_EQ(td::vector<td::int64>({1, 3}), completed);
  ASSERT_TRUE(pending.take(1).is_error());
  ASSERT_TRUE(pending.take(3).is_error());
  pending.remove_if([](auto &) { return true; }, finish);
  ASSERT_EQ(td::vector<td::int64>({1, 3, 2}), completed);
  ASSERT_TRUE(pending.empty());
  ASSERT_TRUE(pending.take(2).is_error());
}
