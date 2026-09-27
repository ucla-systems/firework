CXX = g++

CXXFLAGS = -Wall -Wextra -no-pie -g -O3

APP = unshare

SRC_DIR = .
FIREWORK_RUNTIME_INC_DIR = ../runtime/lib
FIREWORK_RUNTIME_DIR = ../runtime/build/lib

SRC = $(SRC_DIR)/$(APP).cpp

TARGETS = $(APP)

LIB_FIREWORK_RUNTIME = -L$(FIREWORK_RUNTIME_DIR) -Wl,-rpath,$(FIREWORK_RUNTIME_DIR) -lfirework
INC = -I$(FIREWORK_RUNTIME_INC_DIR)

all: $(TARGETS)

$(APP): $(SRC)
	$(CXX) $(CXXFLAGS) $(INC) $(SRC) -o $(APP) $(LIB_FIREWORK_RUNTIME) -lnuma

clean:
	rm -f $(TARGETS)