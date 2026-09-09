# Makefile for Assignment 2 - The Socket Exchange
#
# Works with FreeBSD's make and with GNU make on macOS.
#
#   make        build everything into bin/
#   make clean  remove the built files

CXX = c++
CXXFLAGS = -std=c++11 -Wall -Wextra -O2

all: bin/exchange_server bin/trader_client bin/market_data_client bin/conn_flood
	chmod +x server/run-server client/run-trader client/run-market-data

bin/exchange_server: src/server.cpp src/common.cpp src/common.h
	mkdir -p bin
	$(CXX) $(CXXFLAGS) -o bin/exchange_server src/server.cpp src/common.cpp

bin/conn_flood: src/conn_flood.cpp src/common.cpp src/common.h
	mkdir -p bin
	$(CXX) $(CXXFLAGS) -o bin/conn_flood src/conn_flood.cpp src/common.cpp

bin/trader_client: src/trader_client.cpp src/common.cpp src/common.h
	mkdir -p bin
	$(CXX) $(CXXFLAGS) -o bin/trader_client src/trader_client.cpp src/common.cpp

bin/market_data_client: src/market_data_client.cpp src/common.cpp src/common.h
	mkdir -p bin
	$(CXX) $(CXXFLAGS) -o bin/market_data_client src/market_data_client.cpp src/common.cpp

clean:
	rm -rf bin

.PHONY: all clean