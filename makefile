
MALLOB_BASE_DIRECTORY?='"."'

all: libipasirmallob.a

mallob_ipasir.o: src/mallob_ipasir.cpp src/mallob_ipasir.hpp
	g++ -DMALLOB_BASE_DIRECTORY=${MALLOB_BASE_DIRECTORY} -c -std=c++17 src/mallob_ipasir.cpp -Isrc

libipasirmallob.a: mallob_ipasir.o
	ar rvs libipasirmallob.a mallob_ipasir.o
