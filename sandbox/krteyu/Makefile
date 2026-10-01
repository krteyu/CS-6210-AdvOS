CXX      := g++
CXXFLAGS := -std=c++20 -Wall -Wextra -g
HEADERS  := ringbuffer.h include.h
TARGETS  := consumer producer

.PHONY: all clean

all: $(TARGETS)

consumer: consumer.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -o $@ consumer.cpp

producer: producer.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -o $@ producer.cpp

clean:
	rm -f $(TARGETS)
	rm -f /tmp/ring_info.txt