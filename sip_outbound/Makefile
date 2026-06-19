CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -g -O2 \
    -DPJ_IS_LITTLE_ENDIAN=1 -DPJ_IS_BIG_ENDIAN=0 \
    -DPJ_HAS_EPOLL=1 \
    -I/usr/local/include \
    -I/usr/include/pjlib-util-x86_64-unknown-linux-gnu \
    -I/usr/include/pjlib-x86_64-unknown-linux-gnu \
    -I/usr/include/pjmedia-x86_64-unknown-linux-gnu \
    -I/usr/include/pjmedia-codec-x86_64-unknown-linux-gnu \
    -I/usr/include/pjsip-x86_64-unknown-linux-gnu \
    -I/usr/include/pjsip-simple-x86_64-unknown-linux-gnu \
    -I/usr/include/pjsip-ua-x86_64-unknown-linux-gnu \
    -I/usr/include/pjnath-x86_64-unknown-linux-gnu \
    -I/usr/include/pistache \
    -I/usr/include/mysql \
    -I./include

LDFLAGS = -L/usr/local/lib -L/usr/lib/x86_64-linux-gnu
LIBS = -lpjsua2 -lpjsua \
    -lpjsip-ua \
    -lpjsip-simple \
    -lpjsip \
    -lpjmedia-codec \
    -lpjmedia \
    -lpjmedia-audiodev \
    -lpjmedia-videodev \
    -lpjnath \
    -lpjlib-util \
    -lsrtp \
    -lresample \
    -lpj \
    -lpistache -lboost_system -lssl -lcrypto -pthread -lspandsp -lcurl -lssl -lcrypto -lmysqlclient

SRC_DIR = src
SOURCES = main.cpp \
    $(SRC_DIR)/account.cpp \
    $(SRC_DIR)/call.cpp \
    $(SRC_DIR)/call_manager.cpp \
    $(SRC_DIR)/audio_port.cpp \
    $(SRC_DIR)/audio_recorder.cpp \
    $(SRC_DIR)/websocket_handler.cpp \
    $(SRC_DIR)/ami_manager.cpp \
    $(SRC_DIR)/webhook_manager.cpp \
    $(SRC_DIR)/upload_manager.cpp \
    $(SRC_DIR)/loadbalancer.cpp \
    $(SRC_DIR)/human_ringing.cpp \
    $(SRC_DIR)/simulated_call.cpp \
    $(SRC_DIR)/db_manager.cpp

OBJECTS = $(SOURCES:.cpp=.o)
TARGET = sip_outbound

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $(OBJECTS) -o $(TARGET) $(LDFLAGS) $(LIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET)
