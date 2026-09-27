CXX = g++

FIG6_FLAGS ?=
CXXFLAGS = -Wall -Wextra -no-pie -g -O3

SRC_DIR = .
FIREWORK_RUNTIME_INC_DIR = ../runtime/lib
FIREWORK_RUNTIME_DIR = ../runtime/build/lib

SRC = $(SRC_DIR)/dataframe.cpp

TARGETS = dataframe

LIB_FIREWORK_RUNTIME = -L$(FIREWORK_RUNTIME_DIR) -Wl,-rpath,$(FIREWORK_RUNTIME_DIR) -lfirework -lcryptopp -lsnappy
INC = -I$(FIREWORK_RUNTIME_INC_DIR)

all: $(TARGETS)

dataframe: $(SRC_DIR)/dataframe.cpp
	$(CXX) $(CXXFLAGS) $(FIG6_FLAGS) $(INC) $(SRC_DIR)/dataframe.cpp -o dataframe $(LIB_FIREWORK_RUNTIME)

clean:
	rm -f $(TARGETS)