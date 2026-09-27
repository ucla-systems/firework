CXX = g++

CXXFLAGS = -Wall -Wextra -no-pie -g

SRC_DIR = .
FIREWORK_RUNTIME_INC_DIR = ../runtime/lib
FIREWORK_RUNTIME_DIR = ../runtime/build/lib

SRC = $(SRC_DIR)/locktest.cpp

TARGETS = locktest

LIB_FIREWORK_RUNTIME = -L$(FIREWORK_RUNTIME_DIR) -Wl,-rpath,$(FIREWORK_RUNTIME_DIR) -lfirework -lcryptopp -lsnappy
INC = -I$(FIREWORK_RUNTIME_INC_DIR)

all: $(TARGETS)

locktest: $(SRC_DIR)/locktest.cpp
	$(CXX) $(CXXFLAGS) $(INC) $(SRC_DIR)/locktest.cpp -o locktest $(LIB_FIREWORK_RUNTIME)

clean:
	rm -f $(TARGETS)