# Host tests for the relay's portable core: make -C test
CORE := ../main/core
NODE := ../../node
C5 := ../../node_esp32c5/main
IDF_PATH ?= $(HOME)/esp/esp-idf
CJSON := $(IDF_PATH)/components/json/cJSON
CXXFLAGS := -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined \
    -I$(CORE) -I$(CJSON) -I.
SRCS := test_main.cpp $(CORE)/node_protocol.cpp $(CORE)/node_driver.cpp $(CORE)/race_engine.cpp \
    $(CORE)/channels.cpp $(CORE)/mach1_timer.cpp $(CORE)/feed_url.cpp $(CORE)/web_form.cpp $(CORE)/usb_link.cpp

test_relay: $(SRCS) test_constants.o cJSON.o $(wildcard $(CORE)/*.h) fake_node.h
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS) test_constants.o cJSON.o
	./$@

# Includes the node's own commands.h, which needs the C5 build's shim.
test_constants.o: test_constants.cpp $(CORE)/node_protocol.h $(NODE)/commands.h
	$(CXX) -std=c++17 -DRHNODE_ESP32C5=1 -I$(CORE) -I$(C5) -I$(C5)/shim -I$(NODE) -c -o $@ $<

cJSON.o: $(CJSON)/cJSON.c
	$(CC) -O1 -g -fsanitize=address,undefined -c -o $@ $<

clean:
	rm -f test_relay test_pty *.o
.PHONY: clean

# The driver against the C5 node's real core on a pseudo-terminal (real time, no sanitizers).
PTY_NODE := ../../node_esp32c5/test/pty_node
test_pty: test_pty.cpp $(CORE)/node_driver.cpp $(CORE)/node_protocol.cpp $(wildcard $(CORE)/*.h)
	$(CXX) -std=c++17 -O2 -g -Wall -Wextra -I$(CORE) -o $@ test_pty.cpp $(CORE)/node_driver.cpp $(CORE)/node_protocol.cpp
pty: test_pty
	$(MAKE) -s -C ../../node_esp32c5/test pty_node
	./test_pty $(PTY_NODE)
.PHONY: pty
