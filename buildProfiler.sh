cc -c -O2 z80.c 
cc z80profiler.c z80.o -o z80profiler -lm
strip z80profiler

