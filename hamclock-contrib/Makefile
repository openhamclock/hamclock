# Makefile for hcwebproxy

.PHONY: clean install

CXX = g++
CXXFLAGS = -O2 -g -Wall -pthread
LIBS = -lpthread

hcwebproxy: hcwebproxy.o
	$(CXX) -o hcwebproxy hcwebproxy.o -lpthread

install: hcwebproxy
	sudo mv hcwebproxy /usr/local/bin
	@echo hcwebproxy is now ready in /usr/local/bin

clean:
	rm -fr hcwebproxy.o hcwebproxy
