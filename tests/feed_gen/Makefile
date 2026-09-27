CXX = g++

FIG6_FLAGS ?=
CXXFLAGS = -Wall -Wextra -no-pie -g -O3

SRC_DIR = .
FIREWORK_RUNTIME_INC_DIR = ../runtime/lib
FIREWORK_RUNTIME_DIR = ../runtime/build/lib

SRC = $(SRC_DIR)/feed_gen.cpp

TARGETS = feed_gen

LIB_FIREWORK_RUNTIME = -L$(FIREWORK_RUNTIME_DIR) -Wl,-rpath,$(FIREWORK_RUNTIME_DIR) -lfirework -lcryptopp -lsnappy
INC = -I$(FIREWORK_RUNTIME_INC_DIR)

all: $(TARGETS)

feed_gen: $(SRC_DIR)/feed_gen.cpp
	$(CXX) $(CXXFLAGS) $(FIG6_FLAGS) $(INC) $(SRC_DIR)/feed_gen.cpp -o feed_gen $(LIB_FIREWORK_RUNTIME)

clean:
	rm -f $(TARGETS)