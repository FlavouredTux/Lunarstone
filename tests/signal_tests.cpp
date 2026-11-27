#include "framework.hpp"

#include <lunar/Signal.hpp>

#include <memory>
#include <string>
#include <vector>

using lunar::Connection;
using lunar::Signal;

TEST(signal_passes_arguments)
{
    Signal<int, std::string> signal;
    int gotNumber = 0;
    std::string gotText;
    signal.connect([&](int n, const std::string& s) {
        gotNumber = n;
        gotText = s;
    });
    signal.fire(7, "hi");
    CHECK_EQ(gotNumber, 7);
    CHECK_EQ(gotText, "hi");
}

TEST(signal_runs_handlers_in_connect_order)
{
    Signal<> signal;
    std::vector<int> order;
    signal.connect([&] { order.push_back(1); });
    signal.connect([&] { order.push_back(2); });
    signal.fire();
    CHECK_EQ(order, (std::vector{1, 2}));
}

TEST(signal_disconnect_stops_handler)
{
    Signal<> signal;
    int calls = 0;
    Connection c = signal.connect([&] { ++calls; });
    signal.fire();
    c.disconnect();
    signal.fire();
    CHECK_EQ(calls, 1);
    CHECK(!c.connected());
    CHECK_EQ(signal.connectionCount(), 0u);
}

TEST(signal_disconnect_through_copy)
{
    Signal<> signal;
    Connection a = signal.connect([] {});
    Connection b = a;
    b.disconnect();
    CHECK(!a.connected());
}

TEST(signal_disconnect_during_fire_skips_later_handler)
{
    Signal<> signal;
    Connection second;
    int secondCalls = 0;
    signal.connect([&] { second.disconnect(); });
    second = signal.connect([&] { ++secondCalls; });
    signal.fire();
    CHECK_EQ(secondCalls, 0);
}

TEST(signal_connect_during_fire_waits_for_next_fire)
{
    Signal<> signal;
    int lateCalls = 0;
    bool connectedLate = false;
    signal.connect([&] {
        if (!connectedLate) {
            connectedLate = true;
            signal.connect([&] { ++lateCalls; });
        }
    });
    signal.fire();
    CHECK_EQ(lateCalls, 0);
    signal.fire();
    CHECK_EQ(lateCalls, 1);
}

TEST(signal_connection_outlives_signal)
{
    Connection c;
    {
        Signal<> signal;
        c = signal.connect([] {});
        CHECK(c.connected());
    }
    CHECK(!c.connected());
    c.disconnect(); // must be harmless
}

TEST(signal_disconnect_all)
{
    Signal<> signal;
    int calls = 0;
    Connection a = signal.connect([&] { ++calls; });
    Connection b = signal.connect([&] { ++calls; });
    signal.disconnectAll();
    signal.fire();
    CHECK_EQ(calls, 0);
    CHECK(!a.connected());
    CHECK(!b.connected());
}
