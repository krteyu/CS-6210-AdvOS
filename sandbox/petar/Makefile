CXX      := g++
CXXFLAGS := -std=c++20 -Wall -Wextra -g
TARGETS  := server client

.PHONY: all clean

all: $(TARGETS)

server: server.cpp link.h
	$(CXX) $(CXXFLAGS) -o $@ server.cpp

client: client.cpp link.h
	$(CXX) $(CXXFLAGS) -o $@ client.cpp

clean:
	rm -f $(TARGETS)
	rm -f /tmp/petar_info.txt
