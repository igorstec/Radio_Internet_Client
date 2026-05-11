CXX := g++
CXXFLAGS := -Wall -Wextra -O2 -std=c++17
LDFLAGS =

TARGETS = sikradio test_url 

# g++ -MM *.cpp

.PHONY : all clean

all: $(TARGETS)

#zaleznosci
radio_client.o: radio_client.cpp radio_client_config.h
radio_client_config.o: radio_client_config.cpp radio_client_config.h
test_url.o: test_url.cpp radio_client_config.h


radio_client.o: radio_client.cpp radio_client_config.h
	$(CXX) $(CXXFLAGS) -c radio_client.cpp
sikradio: main.o parser.o sieci.o radio_client.o
	$(CXX) $(CXXFLAGS) -o sikradio main.o parser.o sieci.o radio_client.o

test_url: test_url.o radio_client_config.o
	$(CXX) $(CXXFLAGS) -o test_url test_url.o radio_client_config.o

clean: 
	rm -f $(TARGETS) *.o
