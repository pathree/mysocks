
CXXFLAGS+= -Wall -g -fPIC -std=c++11
LDFLAGS+= -levent

all : server client le-proxy

.PHONY : clean
 
server : server.o utils.o
	$(CXX) -o $@ $(CXXFLAGS) $(LDFLAGS) $^
 
client : client.o utils.o
	$(CXX) -o $@ $(CXXFLAGS) $(LDFLAGS) $^
 
le-proxy : le-proxy.o utils.o
	$(CXX) -o $@ $(CXXFLAGS) $(LDFLAGS) $^
 
clean : 
	rm    server 
	rm    le-proxy
	rm    client 
	rm    *.o
