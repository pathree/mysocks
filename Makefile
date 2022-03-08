
CXXFLAGS+= -Wall -g -fPIC -std=c++11
HEADERS+=   $(wildcard *.h)
LDFLAGS+= -levent

all : server client le-proxy

.PHONY : clean
 
server : server.o utils.o $(HEADERS)
	$(CXX) -o $@ $(CXXFLAGS) $(LDFLAGS) $^
 
client : client.o utils.o $(HEADERS)
	$(CXX) -o $@ $(CXXFLAGS) $(LDFLAGS) $^
 
le-proxy : le-proxy.o utils.o stream.o $(HEADERS)
	$(CXX) -o $@ $(CXXFLAGS) $(LDFLAGS) $^
 
clean : 
	rm    server 
	rm    le-proxy
	rm    client 
	rm    *.o
