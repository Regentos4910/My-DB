CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -I.

.PHONY: all server test run clean

all: server

server:
	$(CXX) $(CXXFLAGS) src/server_main.cpp -o mydb_server

test_btree: tests/test_btree.cpp src/*.h
	$(CXX) $(CXXFLAGS) tests/test_btree.cpp -o test_btree

test_table: tests/test_table.cpp src/*.h
	$(CXX) $(CXXFLAGS) tests/test_table.cpp -o test_table

test_sql: tests/test_sql.cpp src/*.h
	$(CXX) $(CXXFLAGS) tests/test_sql.cpp -o test_sql

test: test_btree test_table test_sql
	./test_btree
	./test_table
	./test_sql

run: server
	./mydb_server mydb_data.db 8080 web

clean:
	rm -f mydb_server test_btree test_table test_sql *.db
