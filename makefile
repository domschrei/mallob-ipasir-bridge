
all: libipasirmallob.a

mallob_ipasir.o: src/mallob_ipasir.cpp src/mallob_ipasir.hpp src/timer.cpp
	g++ -c -std=c++17 -O3 src/mallob_ipasir.cpp src/timer.cpp -Isrc

libipasirmallob.a: mallob_ipasir.o
	ar rvs libipasirmallob.a mallob_ipasir.o timer.o
