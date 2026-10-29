# Host-built engine tests (no TV needed): make -C vega/hcengine/test/host
ENGINE   := ../../kepler/engine
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter
OUT      ?= engine_test

.PHONY: test clean
test: $(OUT)
	./$(OUT)

$(OUT): engine_test.cpp $(ENGINE)/MdnsResponder.cpp $(ENGINE)/LanFilter.cpp $(ENGINE)/Settings.cpp $(ENGINE)/EngineHost.h
	$(CXX) $(CXXFLAGS) -I$(ENGINE) -o $@ engine_test.cpp $(ENGINE)/LanFilter.cpp $(ENGINE)/Settings.cpp -lpthread

clean:
	rm -f $(OUT)
