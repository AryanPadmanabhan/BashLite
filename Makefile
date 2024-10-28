CFLAGS = -Wall -Werror -g
CC = gcc $(CFLAGS)
AN = proj2
SHELL = /bin/bash
CWD = $(shell pwd | sed 's/.*\///g')

bash: bash.o string_vector.o job_list.o bash_funcs.o
	$(CC) -o $@ $^

bash.o: bash.c
	$(CC) -c $^

job_list.o: job_list.c job_list.h
	$(CC) -c $<

string_vector.o: string_vector.c string_vector.h
	$(CC) -c $<

bash_funcs.o: bash_funcs.c
	$(CC) -c $<

clean:
	rm -f *.o bash 

