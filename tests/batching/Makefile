CXX = g++
CXXFLAGS = -Wall -Wextra -no-pie -g -O3

FIREWORK_RUNTIME_INC_DIR = ../runtime/lib
FIREWORK_RUNTIME_DIR = ../runtime/build/lib
LIB_FIREWORK_RUNTIME = -L$(FIREWORK_RUNTIME_DIR) -Wl,-rpath,$(FIREWORK_RUNTIME_DIR) -lfirework
INC = -I$(FIREWORK_RUNTIME_INC_DIR)

all: batching

batching: batching.cpp
	$(CXX) $(CXXFLAGS) $(INC) batching.cpp -o batching $(LIB_FIREWORK_RUNTIME)

clean:
	rm -f batching
