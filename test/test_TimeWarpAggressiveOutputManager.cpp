#define CATCH_CONFIG_MAIN  // This tells Catch to provide a main()
#include "catch.hpp"

#include "TimeWarpAggressiveOutputManager.hpp"
#include "mocks.hpp"
#include "utility/memory.hpp"

TEST_CASE("Aggressive output manager is aggressive", "[output][queues]") {
    unsigned int num_objects = 4;
    warped::TimeWarpAggressiveOutputManager om;
    om.initialize(num_objects);
    std::shared_ptr<warped::Event> e = std::make_shared<test_Event>();

    SECTION("Output queues are constructed correctly", "[output][queues]") {
        for (unsigned int i = 0; i < num_objects; i++) {
            REQUIRE(om.size(i) == 0);
        }
    }

    SECTION("Add a series of event to an output queue", "[output][queue][add]") {
        om.insertEvent(warped::make_unique<test_Event>("receiver_name", 27), 2);
        REQUIRE(om.size(2) == 1);

        om.insertEvent(warped::make_unique<test_Event>("receiver_name", 29), 2);
        REQUIRE(om.size(2) == 2);

        om.insertEvent(warped::make_unique<test_Event>("receiver_name", 33), 2);
        om.insertEvent(warped::make_unique<test_Event>("receiver_name", 33), 2);
        om.insertEvent(warped::make_unique<test_Event>("receiver_name", 38), 2);
        om.insertEvent(warped::make_unique<test_Event>("receiver_name", 40), 2);
        om.insertEvent(warped::make_unique<test_Event>("receiver_name", 45), 2);
        REQUIRE(om.size(2) == 7);

        SECTION("Fossil Collection removes correct events", "[output][queue][fossilCollection]") {
            unsigned int gvt = 35;
            unsigned int lts = om.fossilCollect(gvt, 2);
            CHECK(om.size(2) == 3);
            CHECK(lts == 38);
        }

        SECTION("Correct events are removed on rollback", "[output][queue][rollback]") {
            dynamic_cast<test_Event*>(e.get())->receive_time_ = 39;
            auto events_to_cancel = om.rollback(e, 2);
            CHECK(om.size(2) == 5);
            REQUIRE(events_to_cancel->size() == 2);

            auto event = std::move(events_to_cancel->back());
            CHECK(event->timestamp() == 40);

            events_to_cancel->pop_back();
            event = std::move(events_to_cancel->back());
            CHECK(event->timestamp() == 45);
        }
    }
}

