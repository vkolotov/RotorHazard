# Host tests: the C5 meter, scanner and glue with RotorHazard's own node core.
NODE := ../../node
MAIN := ../main
CXXFLAGS := -std=c++17 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-type-limits \
    -DRHNODE_ESP32C5=1 -I$(MAIN) -I$(MAIN)/shim -I$(NODE) -I.
SRCS := test_main.cpp $(MAIN)/c5_meter.cpp $(MAIN)/c5_scan.cpp $(MAIN)/c5_rhglue.cpp \
    $(NODE)/RssiNode.cpp $(NODE)/commands.cpp

test_c5: $(SRCS) $(wildcard $(MAIN)/*.h) sim_radio.h
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)
	./$@

clean:
	rm -f test_c5 pty_node
.PHONY: clean

PTY_SRCS := pty_node.cpp $(MAIN)/c5_meter.cpp $(MAIN)/c5_scan.cpp $(MAIN)/c5_rhglue.cpp \
    $(NODE)/RssiNode.cpp $(NODE)/commands.cpp
pty_node: $(PTY_SRCS) $(wildcard $(MAIN)/*.h) sim_radio.h
	$(CXX) $(CXXFLAGS) -o $@ $(PTY_SRCS)
