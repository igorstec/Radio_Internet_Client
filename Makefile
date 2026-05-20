TARGET := sikradio
CXX ?= g++
PKG_CONFIG ?= pkg-config

SRCS := radio_client.cpp radio_client_config.cpp radio_http.cpp
OBJS := $(SRCS:.cpp=.o)

CXXFLAGS ?= -std=c++20 -Wall -Wextra -Wpedantic -O2

OPENSSL_CFLAGS := $(shell $(PKG_CONFIG) --cflags openssl 2>/dev/null)
OPENSSL_LIBS   := $(shell $(PKG_CONFIG) --libs openssl 2>/dev/null)

ifeq ($(strip $(OPENSSL_LIBS)),)
OPENSSL_LIBS := -lssl -lcrypto
endif

CPPFLAGS += $(OPENSSL_CFLAGS)
LDLIBS   += $(OPENSSL_LIBS)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) -o $@ $(LDLIBS)

%.o: %.cpp radio_client_config.h radio_http.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean