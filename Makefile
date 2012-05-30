CXX = g++
OBJS = main.o amf.o utils.o
CXXFLAGS = -W -Wall -O2 -g

server: $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS)
