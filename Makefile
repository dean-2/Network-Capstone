CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -O2
CPPFLAGS ?= -Iinclude

all: bserve bcurL

bserve: src/server.cpp src/file_server.cpp src/protocol.cpp include/protocol.hpp include/file_server.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/server.cpp src/file_server.cpp src/protocol.cpp -o $@

bcurL: src/client.cpp src/protocol.cpp include/protocol.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/client.cpp src/protocol.cpp -o $@

tests/test_protocol: tests/test_protocol.cpp src/protocol.cpp include/protocol.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -pthread tests/test_protocol.cpp src/protocol.cpp -o $@

test: all tests/test_protocol
	./tests/test_protocol
	python3 tests/test_integration.py

.PHONY: all test clean

clean:
	rm -f tests/test_protocol bserve bcurL
