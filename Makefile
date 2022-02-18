
CXXFLAGS+= -std=c++11 -Wall -fPIC  -g -levent

all : server client le-proxy

.PHONY : clean
 
server : server.o utils.o
	$(CXX) -o $@ $(CXXFLAGS) $^
 
client : client.o utils.o
	$(CXX) -o $@ $(CXXFLAGS) $^
 
le-proxy : le-proxy.o utils.o
	$(CXX) -o $@ $(CXXFLAGS) $^
 
clean : 
	rm    server 
	rm    le-proxy
	rm    client 
	rm    *.o
