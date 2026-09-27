CXX = g++

FIG6_FLAGS ?=
CXXFLAGS = -Wall -Wextra -no-pie -g -O3

SRC_DIR = .
FIREWORK_RUNTIME_INC_DIR = ../runtime/lib
FIREWORK_RUNTIME_DIR = ../runtime/build/lib

SRC = $(SRC_DIR)/pagerank.cpp

TARGETS = pagerank

LIB_FIREWORK_RUNTIME = -L$(FIREWORK_RUNTIME_DIR) -Wl,-rpath,$(FIREWORK_RUNTIME_DIR) -lfirework
INC = -I$(FIREWORK_RUNTIME_INC_DIR)

all: $(TARGETS)

pagerank: $(SRC_DIR)/pagerank.cpp
	$(CXX) $(CXXFLAGS) $(FIG6_FLAGS) $(INC) $(SRC_DIR)/pagerank.cpp -o pagerank $(LIB_FIREWORK_RUNTIME)

clean:
	rm -f $(TARGETS)