all: clean simple_server simple_client

simple_server: simple_server.c
	gcc -o $@ -Wall $^ -std=c99 -g -libverbs -lrdmacm

simple_client: simple_client.c simple_common.c
	gcc -o $@ -Wall $^ -std=c99 -g -libverbs -lrdmacm

clean:
	rm -f simple_server simple_client
