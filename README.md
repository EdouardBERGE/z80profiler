# z80profiler
This Z80 profiler is able to load, run and time any Z80 binary.

This Z80 profiler may use screening process to validate robustness of a programm and/or easily test 

This Z80 profiler is able to compute Branch coverage and RAW memory coverage


z80 emulation based on work from Nicolas Allemand

## Usage

Run `z80profiler` without any option to display this help.

```
===========   Z80 profiler   ===========
Emulation based on Nicolas Allemand work

syntax : emuZ80 <filename> <options>

load options :
-fileOffset <value>         to skip first bytes
-fileLength <value>         to truncate reading
-destOffset <value>         read at memory address

emulation options :
-VBL                        generate VBL signal
-VBLdelay <value>           delay between each VBL
-INT                        generate INT signal
-INTdelay <value>[,<value>] delay(s) between each INT
-dirt 0|1|2                 0: use clean memory
                            1: use CPC like memory
                            2: use shuffled memory
-IOCPC                      mimic CPC input/output

emulation init options :
-pc <value>                 set PC value when starting
                            ex: -pc 0x100
-<reg> <value>              set register value
                            ex: -bc 0x100
     reg may be : a,f,b,c,d,e,h,l,xl,xh,yl,yh
                  af,bc,de,hl,ix,iy
                  afp,bcp,dep,hlp,ap,fp,bp,cp,dp,ep,hp,lp
-crible <reg>,<reg>,...     crible registers in the list
-addVec38                   add EI:RET in #38

run options :
-maxnop <value>             RUN maximum duration in nops
-maxCycle <value>           RUN maximum duration in cycles
-maxRun <value>             RUN multiple times
-stopAddress <value>        RUN until reaching address (disabled by default)
-eiStop                     RUN until EI
-haltStop                   RUN until HALT
-unknownStop                RUN until unknown instruction
-debug                      ultra verbose information (for debug purpose)

result options (values: decimal, 0x1234 or #1234) :
-expect <reg>=<value>       check a register when RUN ends (repeatable)
                            ex: -expect hl=#1234 -expect a=0
-expectMem <addr>,<b>[,<b>] check consecutive bytes when RUN ends (repeatable)
-dump <addr>,<length>       hexa dump of memory when RUN ends (repeatable)
-dumpFile <addr>,<len>,<f>  write memory to a binary file when RUN ends

report (end of output, one tag per line) :
STOP: <reason>              HALT, EI, UNKNOWN, STOPADDRESS, MAXNOP, MAXCYCLE
RUN: / INPUT: / INALT:      with -maxRun, run count and starting registers
REGS: / ALT :               registers when RUN ends, flags as SZ5H3VNC
EXPECT OK | EXPECT FAIL:    result of -expect / -expectMem
MEM #xxxx: / DUMP           result of -dump / -dumpFile
EXIT: <code>                also the process exit code

exit codes : 0 ok, 1 bad parameter, 2 file error, 3 nop/cycle limit reached,
             4 unknown instruction, 5 expectation failed
with -maxRun, runs stop at the first failing one
```
