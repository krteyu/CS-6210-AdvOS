CXX      := g++
CXXFLAGS := -std=c++20 -Wall -Wextra -g -MMD -MP

SRC_DIR     := src
COMMON_DIR  := $(SRC_DIR)/common
SERVER_DIR  := $(SRC_DIR)/server
CLIENT_DIR  := $(SRC_DIR)/client
APP_DIR     := $(SRC_DIR)/app
SNAPPY_DIR  := snappy
TEST_RUN	:= test_runs

BUILD_DIR   := build
OUT_DIR		:= out
BIN_DIR     := .

INCLUDES := -I$(COMMON_DIR) -I$(SERVER_DIR) -I$(CLIENT_DIR) -I$(SNAPPY_DIR)

# ---- Source discovery ----
COMMON_SRCS := $(wildcard $(COMMON_DIR)/*.cpp)
SERVER_SRCS := $(wildcard $(SERVER_DIR)/*.cpp)
CLIENT_SRCS := $(wildcard $(CLIENT_DIR)/*.cpp)
APP_SRCS    := $(wildcard $(APP_DIR)/*.cpp)

# ---- Object files (mirrored under build/) ----
COMMON_OBJS := $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(COMMON_SRCS))
SERVER_OBJS := $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(SERVER_SRCS))
CLIENT_OBJS := $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(CLIENT_SRCS))
APP_OBJS    := $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(APP_SRCS))

DEPS := $(COMMON_OBJS:.o=.d) $(SERVER_OBJS:.o=.d) $(CLIENT_OBJS:.o=.d) $(APP_OBJS:.o=.d)

LIB_TINYFILE := $(BUILD_DIR)/libtinyfile.a

.PHONY: all build server app clean

all: build

build: server app

server: $(BIN_DIR)/tinyfile-server

app: $(LIB_TINYFILE) $(BIN_DIR)/sample-app

# ---- Server binary: common + server objects ----
$(BIN_DIR)/tinyfile-server: $(COMMON_OBJS) $(SERVER_OBJS) $(BUILD_DIR)/snappy.o
	$(CXX) $(CXXFLAGS) -o $@ $^

# ---- Client static library: common + client objects ----
$(LIB_TINYFILE): $(COMMON_OBJS) $(CLIENT_OBJS)
	ar rcs $@ $^

# ---- Sample app: links against libtinyfile.a ----
$(BIN_DIR)/sample-app: $(APP_OBJS) $(LIB_TINYFILE)
	$(CXX) $(CXXFLAGS) -o $@ $(APP_OBJS) -L$(BUILD_DIR) -ltinyfile

# ---- Generic compile rule, mirrors src/ tree into build/ ----
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/snappy.o: $(SNAPPY_DIR)/snappy.c $(SNAPPY_DIR)/snappy.h $(SNAPPY_DIR)/compat.h
	@mkdir -p $(dir $@)
	gcc -std=gnu11 -O2 -I$(SNAPPY_DIR) -c $< -o $@

-include $(DEPS)

clean:
	rm -rf $(BUILD_DIR)
	rm -f $(BIN_DIR)/tinyfile-server $(BIN_DIR)/sample-app
	rm -rf $(OUT_DIR)
	rm -f /tmp/tinyfile_server_info.txt

clean_runs:
	rm -rf $(TEST_RUN)