# Makefile — Linux / g++ build (the spec's deploy target: "a normal Ubuntu
# laptop"). Mirrors build.ps1 on Windows. Header-only library => each app is a
# single translation unit.
#
#   make            # build everything into ./build
#   make tests && ./build/tests
#   make bench  && ./build/bench --orders 1000000

CXX      ?= g++
# -O3 -march=native: the spec's hot-path flags. -fno-rtti matches MSVC /GR-.
# We keep exceptions (the STL uses them; the hot path never throws).
CXXFLAGS ?= -std=c++17 -O3 -march=native -fno-rtti -Iinclude -Wall -Wextra
LDFLAGS  ?= -pthread

BUILD := build
APPS  := replay bench bench_ds pipeline_bench recover_demo feed_server \
         gen_itch itch_replay lobster_replay raft_node
TESTS := tests tests_durability tests_feed tests_raft

APP_SRC  = apps
TEST_SRC = tests

.PHONY: all clean apps tests
all: apps tests

apps:  $(addprefix $(BUILD)/,$(APPS))
tests: $(BUILD)/tests $(BUILD)/tests_durability $(BUILD)/tests_feed

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/tests: $(TEST_SRC)/test_order_book.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

$(BUILD)/tests_durability: $(TEST_SRC)/test_durability.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

$(BUILD)/tests_feed: $(TEST_SRC)/test_feed.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

$(BUILD)/tests_raft: $(TEST_SRC)/test_raft.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

$(BUILD)/%: $(APP_SRC)/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -rf $(BUILD)
