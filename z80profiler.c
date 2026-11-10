#include<math.h>
#include<stdlib.h>
#include<stdio.h>
#include<string.h>
#include<ctype.h>
#include "z80.h"
#ifdef OS_WIN
#include<fcntl.h>
#include<io.h>
#endif

#ifndef stricmp
// Linus Torvald (c) 1991
int stricmp(const char *s1, const char *s2) {
/* Yes, Virginia, it had better be unsigned */
unsigned char c1, c2;
while (1) {
    c1 = *s1++;
    c2 = *s2++;
    if (!c1 || !c2)
        break; 
    if (c1 == c2)
        continue;
    c1 = tolower(c1);
    c2 = tolower(c2);
    if (c1 != c2)
        break;
}
return (int)c1 - (int)c2;
}
#endif

void FieldArrayAddDynamicValueConcat(char ***zearray, int *nbfields, int *maxfields, char *zevalue)
{
        if ((*zearray)==NULL) {
                *nbfields=1;
                *maxfields=10;
                (*zearray)=realloc(NULL,(*maxfields)*sizeof(char *));
        } else {
                *nbfields=(*nbfields)+1;
                if (*nbfields>=*maxfields) {
                        *maxfields=(*maxfields)*2;
                        (*zearray)=realloc((*zearray),(*maxfields)*sizeof(char *));
                }
        }
        /* using direct calls because it is more interresting to know which is the caller */
        (*zearray)[(*nbfields)-1]=strdup(zevalue);
        (*zearray)[(*nbfields)]=NULL;
}
char **TxtSplitWithChar(char *in_str, char split_char)
{
        char **tab=NULL;
        char *match_str;
        int redo=1;
        int idx,idmax;

        match_str=in_str;

        while (redo && *in_str) {
                while (*match_str && *match_str!=split_char) match_str++;
                redo=*match_str;
                *match_str=0;
                FieldArrayAddDynamicValueConcat(&tab,&idx,&idmax,in_str);
                in_str=++match_str;
        }

        return tab;
}

struct s_check; // expectations and dumps, see "End of run report"

struct s_parameter {
	FILE *f;
	char *filename;
	char *crible;

	unsigned int fileOffset,fileLength; // if u dunna want to use all the file
	unsigned int destOffset;
	unsigned int runAt; // offset for the Z80 emulator
	int stopAddress;

	int useMaxNop,useMaxCycle;
	unsigned int maxNop,maxCycle,maxRun;

	int addVec38;
	int genVBL;      // 0:no(default) 1:yes
	int genVBLdelay; // default 312*64 nops
	int genINT;      // 0:no(default) 1:yes
	int genINTdelay; // default 52*64 nops
	int dirtMemory;  // 0:none(default) 1:cpc 2:dirt
	int ioCPC;       // 0:no(default) 1:try to mimic CPC
	int haltStop;
	int eiStop;
	int unknownStop;
	int debug;

	int a,b,c,d,e,regF,h,l,xl,xh,yl,yh,af,bc,de,hl,ix,iy,afp,bcp,dep,hlp,ap,fp,bp,cp,dp,ep,hp,lp,sp;

	struct s_check *check;
};

unsigned char memory[65536];
unsigned long hasVBL=0;

#define IS_PPI(port)        ((port & 0x0800) == 0x0000)
uint8_t cpc_wrapin(z80* const z, uint8_t port) {
  // CPC port relies on BC value :)
  unsigned char retVal=0xFF;
  unsigned short int bc;

  bc=(z->b<<8)|z->c;
  if (IS_PPI(bc)) {
	  if ((bc&0x300)==0x100) {
		  // PPI port B
		  retVal=0x10; // 50Hz
		  if (z->nop<hasVBL) {
			  retVal|=1;
		  }
	  }
  }

  return retVal;
}
void cpc_wrapout(z80* const z, uint8_t port, uint8_t val) {
	// pouet pouet
}

/*********************************************************************
 * crible: try many register values over the RUNs
 * each crible byte is a digit of an odometer, a digit walks the first
 * ncrible values of crible[] (ordered to spread values as much as possible)
 *********************************************************************/
#define MAX_CRIBLE_BYTES 32

unsigned char crible[256];      // values, in trial order
unsigned int ncrible;           // number of values tried per byte
unsigned int icrible;           // next byte read in the current RUN
unsigned int mcrible;           // number of crible bytes (a 16 bits register counts twice)
int rcrible[MAX_CRIBLE_BYTES]={0}; // odometer: value index of each byte

void initialize_crible(int maxrun) {
	int i=0,j,k;
	int vmax,imax,vmin,imin;

	crible[i++]=0;
	crible[i++]=255;
	crible[i++]=127;
	crible[i++]=128;
	crible[i++]=1;
	crible[i++]=2;
	for (;i<256;i++) {
		vmax=0;
		for (j=0;j<256;j++) {
			for (k=0;k<i;k++) {
				if (crible[k]==j) break; // already assigned
			}
			if (k<i) continue; // skip
					   //
			// find minimum
			vmin=256;
			for (k=0;k<i;k++) {
				if (abs(crible[k]-j)<vmin) {
					vmin=abs(crible[k]-j);
					imin=j;
				}
			}
			// keep max :)
			if (vmin>vmax) {
				imax=imin;
				vmax=vmin;
			}
		}
		// value far away from previous values
		crible[i]=imax;
	}
	// crible distribution ok
	//for (i=0;i<256;i++) printf("#%02X ",crible[i]);
	//printf("\n");

	if (mcrible>MAX_CRIBLE_BYTES) {
		fprintf(stderr,"ERROR : too many registers to crible (%d bytes max)\n",MAX_CRIBLE_BYTES);
		exit(1);
	}
	// as many values per byte as possible, all combinations fitting in maxrun
	ncrible=2;
	while (ncrible<256 && pow(ncrible+1,mcrible)<=maxrun) ncrible++;
	printf("crible will target %d different values per register byte (according to maxrun value)\n",ncrible);
	memset(rcrible,0,sizeof(rcrible));
	icrible=0;
}

// value of the next crible byte for the current RUN
unsigned char crible8() {
	return crible[rcrible[icrible++]];
}

// advance the odometer once per RUN: first byte moves, carry to the next ones
void crible_next_run() {
	unsigned int digit;

	icrible=0;
	for (digit=0;digit<mcrible;digit++) {
		rcrible[digit]=(rcrible[digit]+1)%ncrible;
		if (rcrible[digit]) break; // no carry
	}
}

unsigned short int crible16() {
	unsigned char high=crible8(); // two statements: C does not order calls inside an expression
	unsigned char low=crible8();
	return (high<<8)|low;
}

/*********************************************************************
 * End of run report: stop reason, registers, expectations, dumps
 *********************************************************************/

// process exit codes (1 and 2 are already used for parameter/file errors)
#define EXIT_RUN_OK      0
#define EXIT_RUN_LIMIT   3
#define EXIT_RUN_UNKNOWN 4
#define EXIT_RUN_EXPECT  5

// why a RUN ended
enum e_stop {
	STOP_HALT,
	STOP_EI,
	STOP_UNKNOWN,
	STOP_ADDRESS,
	STOP_MAXNOP,
	STOP_MAXCYCLE
};

// snapshot of every register, small enough to be copied at each run
struct s_regs {
	uint8_t a,f,b,c,d,e,h,l;
	uint8_t a_,f_,b_,c_,d_,e_,h_,l_;
	uint16_t ix,iy,sp,pc;
	uint8_t i,r,im,iff1,iff2;
};

void CaptureRegs(z80 *z, struct s_regs *regs) {
	regs->a=z->a; regs->f=z80_get_f(z);
	regs->b=z->b; regs->c=z->c; regs->d=z->d; regs->e=z->e; regs->h=z->h; regs->l=z->l;
	regs->a_=z->a_; regs->f_=z->f_;
	regs->b_=z->b_; regs->c_=z->c_; regs->d_=z->d_; regs->e_=z->e_; regs->h_=z->h_; regs->l_=z->l_;
	regs->ix=z->ix; regs->iy=z->iy; regs->sp=z->sp; regs->pc=z->pc;
	regs->i=z->i; regs->r=z->r; regs->im=z->interrupt_mode;
	regs->iff1=z->iff1; regs->iff2=z->iff2;
}

// F decoded as SZ5H3VNC, a dot for each flag at zero
void FlagsToString(uint8_t f, char *str) {
	const char *letters="SZ5H3VNC";
	int bit;
	for (bit=0;bit<8;bit++) str[bit]=(f&(0x80>>bit))?letters[bit]:'.';
	str[8]=0;
}

// two lines, each one starting with a fixed tag (easy to grep)
void PrintRegs(const char *tag, const char *altTag, struct s_regs *regs) {
	char flags[9];

	FlagsToString(regs->f,flags);
	printf("%s AF=#%02X%02X BC=#%02X%02X DE=#%02X%02X HL=#%02X%02X IX=#%04X IY=#%04X SP=#%04X PC=#%04X I=#%02X R=#%02X FLAGS=%s\n",
		tag,regs->a,regs->f,regs->b,regs->c,regs->d,regs->e,regs->h,regs->l,
		regs->ix,regs->iy,regs->sp,regs->pc,regs->i,regs->r,flags);
	printf("%s AF'=#%02X%02X BC'=#%02X%02X DE'=#%02X%02X HL'=#%02X%02X IM=%d IFF1=%d IFF2=%d\n",
		altTag,regs->a_,regs->f_,regs->b_,regs->c_,regs->d_,regs->e_,regs->h_,regs->l_,
		regs->im,regs->iff1,regs->iff2);
}

// value of a register by name (same names as the command line), -1 if unknown
int GetRegByName(struct s_regs *regs, const char *name) {
	if (stricmp(name,"a")==0)   return regs->a;
	if (stricmp(name,"f")==0)   return regs->f;
	if (stricmp(name,"b")==0)   return regs->b;
	if (stricmp(name,"c")==0)   return regs->c;
	if (stricmp(name,"d")==0)   return regs->d;
	if (stricmp(name,"e")==0)   return regs->e;
	if (stricmp(name,"h")==0)   return regs->h;
	if (stricmp(name,"l")==0)   return regs->l;
	if (stricmp(name,"xh")==0)  return regs->ix>>8;
	if (stricmp(name,"xl")==0)  return regs->ix&0xFF;
	if (stricmp(name,"yh")==0)  return regs->iy>>8;
	if (stricmp(name,"yl")==0)  return regs->iy&0xFF;
	if (stricmp(name,"af")==0)  return (regs->a<<8)|regs->f;
	if (stricmp(name,"bc")==0)  return (regs->b<<8)|regs->c;
	if (stricmp(name,"de")==0)  return (regs->d<<8)|regs->e;
	if (stricmp(name,"hl")==0)  return (regs->h<<8)|regs->l;
	if (stricmp(name,"ix")==0)  return regs->ix;
	if (stricmp(name,"iy")==0)  return regs->iy;
	if (stricmp(name,"sp")==0)  return regs->sp;
	if (stricmp(name,"pc")==0)  return regs->pc;
	if (stricmp(name,"ap")==0)  return regs->a_;
	if (stricmp(name,"fp")==0)  return regs->f_;
	if (stricmp(name,"bp")==0)  return regs->b_;
	if (stricmp(name,"cp")==0)  return regs->c_;
	if (stricmp(name,"dp")==0)  return regs->d_;
	if (stricmp(name,"ep")==0)  return regs->e_;
	if (stricmp(name,"hp")==0)  return regs->h_;
	if (stricmp(name,"lp")==0)  return regs->l_;
	if (stricmp(name,"afp")==0) return (regs->a_<<8)|regs->f_;
	if (stricmp(name,"bcp")==0) return (regs->b_<<8)|regs->c_;
	if (stricmp(name,"dep")==0) return (regs->d_<<8)|regs->e_;
	if (stricmp(name,"hlp")==0) return (regs->h_<<8)|regs->l_;
	return -1;
}

// numbers may be decimal, 0x1234 or #1234
unsigned int ParseValue(const char *str) {
	if (str[0]=='#') return strtoul(str+1,NULL,16);
	if (str[0]=='0' && (str[1]=='x' || str[1]=='X')) return strtoul(str+2,NULL,16);
	return strtoul(str,NULL,10);
}

#define MAX_EXPECT 64
#define MAX_DUMP   16

struct s_expectReg { char name[8]; unsigned int value; };
struct s_expectMem { unsigned short int address; unsigned char value; };
struct s_dump      { unsigned short int address; unsigned int length; char *filename; }; // filename NULL => hexa on stdout

struct s_check {
	struct s_expectReg reg[MAX_EXPECT]; int nreg;
	struct s_expectMem mem[MAX_EXPECT]; int nmem;
	struct s_dump dump[MAX_DUMP];       int ndump;
};

// -expect <reg>=<value>
void AddExpectReg(struct s_check *check, char *arg) {
	struct s_regs dummy={0};
	char *sep=strchr(arg,'=');

	if (!sep || sep-arg>=sizeof(check->reg[0].name)) {
		fprintf(stderr,"ERROR : -expect needs <reg>=<value>, got [%s]\n",arg); exit(1);
	}
	if (check->nreg>=MAX_EXPECT) { fprintf(stderr,"ERROR : too many -expect\n"); exit(1); }
	memcpy(check->reg[check->nreg].name,arg,sep-arg);
	check->reg[check->nreg].name[sep-arg]=0;
	if (GetRegByName(&dummy,check->reg[check->nreg].name)<0) {
		fprintf(stderr,"ERROR : unknown register [%s] for -expect\n",check->reg[check->nreg].name); exit(1);
	}
	check->reg[check->nreg].value=ParseValue(sep+1);
	check->nreg++;
}

// -expectMem <address>,<byte>[,<byte>...] : consecutive bytes
void AddExpectMem(struct s_check *check, char *arg) {
	char **list=TxtSplitWithChar(arg,',');
	unsigned int address;
	int i;

	if (!list[0] || !list[1]) { fprintf(stderr,"ERROR : -expectMem needs <address>,<byte>[,<byte>...]\n"); exit(1); }
	address=ParseValue(list[0]);
	for (i=1;list[i];i++) {
		if (check->nmem>=MAX_EXPECT) { fprintf(stderr,"ERROR : too many -expectMem bytes\n"); exit(1); }
		check->mem[check->nmem].address=(address+i-1)&0xFFFF;
		check->mem[check->nmem].value=ParseValue(list[i]);
		check->nmem++;
	}
}

// -dump <address>,<length>  or  -dumpFile <address>,<length>,<file>
void AddDump(struct s_check *check, char *arg, int toFile) {
	char **list=TxtSplitWithChar(arg,',');

	if (!list[0] || !list[1] || (toFile && !list[2])) {
		fprintf(stderr,"ERROR : %s needs <address>,<length>%s\n",toFile?"-dumpFile":"-dump",toFile?",<file>":""); exit(1);
	}
	if (check->ndump>=MAX_DUMP) { fprintf(stderr,"ERROR : too many dumps\n"); exit(1); }
	check->dump[check->ndump].address=ParseValue(list[0]);
	check->dump[check->ndump].length=ParseValue(list[1]);
	check->dump[check->ndump].filename=toFile?list[2]:NULL;
	check->ndump++;
}

// compare registers and memory with expectations, print every mismatch, return number of errors
int CheckExpectations(struct s_check *check, struct s_regs *regs, unsigned char *ram, int verbose) {
	int i,value,nerror=0;

	for (i=0;i<check->nreg;i++) {
		value=GetRegByName(regs,check->reg[i].name);
		if (value!=check->reg[i].value) {
			if (verbose) printf("EXPECT FAIL: %s=#%04X (expected #%04X)\n",check->reg[i].name,value,check->reg[i].value);
			nerror++;
		}
	}
	for (i=0;i<check->nmem;i++) {
		value=ram[check->mem[i].address];
		if (value!=check->mem[i].value) {
			if (verbose) printf("EXPECT FAIL: (#%04X)=#%02X (expected #%02X)\n",check->mem[i].address,value,check->mem[i].value);
			nerror++;
		}
	}
	return nerror;
}

// memory wraps at 64K like the Z80 address bus
void DoDumps(struct s_check *check, unsigned char *ram) {
	unsigned int i,j;
	FILE *f;

	for (i=0;i<check->ndump;i++) {
		struct s_dump *dump=&check->dump[i];
		if (dump->filename) {
			f=fopen(dump->filename,"wb");
			if (!f) { fprintf(stderr,"ERROR : cannot write dump file [%s]\n",dump->filename); continue; }
			for (j=0;j<dump->length;j++) fputc(ram[(dump->address+j)&0xFFFF],f);
			fclose(f);
			printf("DUMP #%04X,%u written to [%s]\n",dump->address,dump->length,dump->filename);
		} else {
			for (j=0;j<dump->length;j++) {
				if ((j&15)==0) printf("%sMEM #%04X:",j?"\n":"",(dump->address+j)&0xFFFF);
				printf(" %02X",ram[(dump->address+j)&0xFFFF]);
			}
			printf("\n");
		}
	}
}

void PrintStop(enum e_stop reason, z80 *z, unsigned short int instrPC) {
	switch (reason) {
		case STOP_HALT:     printf("STOP: HALT at #%04X\n",(z->pc-1)&0xFFFF); break; // PC is already past the HALT
		case STOP_EI:       printf("STOP: EI at #%04X\n",instrPC); break;
		case STOP_UNKNOWN:  printf("STOP: UNKNOWN opcode %02X %02X %02X %02X at #%04X\n",
					z->userdata[instrPC],z->userdata[(instrPC+1)&0xFFFF],
					z->userdata[(instrPC+2)&0xFFFF],z->userdata[(instrPC+3)&0xFFFF],instrPC); break;
		case STOP_ADDRESS:  printf("STOP: STOPADDRESS at #%04X\n",z->pc); break;
		case STOP_MAXNOP:   printf("STOP: MAXNOP limit, next PC=#%04X\n",z->pc); break;
		case STOP_MAXCYCLE: printf("STOP: MAXCYCLE limit, next PC=#%04X\n",z->pc); break;
	}
}

int EmuZ80(struct s_parameter *param) {
	unsigned int zesize;
	unsigned int INTcpt=0,INTcptMax=64*52;
	unsigned int VBLcpt=0,VBLcptMax=64*312;

	int totalCond=0;
	int JRCond=0,JRCondPartial=0;
	int JPCond=0,JPCondPartial=0;
	int RETCond=0,RETCondPartial=0;
	int CALLCond=0,CALLCondPartial=0;
	int rawCoverage=0;

	unsigned long long totalCyc=0;
	unsigned long long totalNop=0;
	unsigned long long totalRun=0;
	unsigned long minRunC,maxRunC;
	unsigned long minRun,maxRun;
	int maxRunDisplay=0;
	char **myreg=NULL;
	int ireg,maxreg=0;
	int i;
	z80 z;

	enum e_stop stopReason=STOP_HALT;
	struct s_regs inputRegs,outputRegs; // registers when RUN starts and ends
	unsigned short int instrPC=0;       // address of the last executed instruction
	int exitCode=EXIT_RUN_OK;

	printf("Load [%s]\n",param->filename);
	if (param->genINTdelay) INTcptMax=param->genINTdelay;
	if (param->genVBLdelay) VBLcptMax=param->genVBLdelay;
	fseek(param->f,0,SEEK_END);
	zesize=ftell(param->f);
	fseek(param->f,param->fileOffset,SEEK_SET);
	zesize-=param->fileOffset; // bytes available after the skipped ones
	if (param->fileLength) zesize=param->fileLength;
	if (zesize>65536) {
		zesize=65536;
		fprintf(stderr,"WARNING: Truncated dataSize to 65536 bytes\n");
	}
	if (zesize+param->destOffset>65536) {
		fprintf(stderr,"ERROR : cannot insert %d bytes in memory at offset %d\n",zesize,param->destOffset);
		exit(2);
	}
	// initialise memory with dirt (or not) BEFORE loading the programm
	switch (param->dirtMemory) {
		default:
		case 0:break;
		case 1:for (i=0;i<65536;i++) memory[i]=(i&0x08)?0xFF:0x00;break;
		case 2:for (i=0;i<65536;i++) memory[i]=rand()&0xFF;break;
	}
	// load the programm at its destination address
	if (fread(memory+param->destOffset,1,zesize,param->f)!=zesize) {
		fprintf(stderr,"ERROR : cannot read %d bytes of [%s] starting at offset %d\n",zesize,param->filename,param->fileOffset);
		exit(2);
	}
	fclose(param->f);

	if (param->crible) {
		myreg=TxtSplitWithChar(param->crible,',');
		for (maxreg=mcrible=0;myreg[maxreg];maxreg++,mcrible++) { // count registers
			if (stricmp(myreg[maxreg],"af")==0) mcrible++; else
			if (stricmp(myreg[maxreg],"bc")==0) mcrible++; else
			if (stricmp(myreg[maxreg],"de")==0) mcrible++; else
			if (stricmp(myreg[maxreg],"hl")==0) mcrible++; else
			if (stricmp(myreg[maxreg],"ix")==0) mcrible++; else
			if (stricmp(myreg[maxreg],"iy")==0) mcrible++; else
			if (stricmp(myreg[maxreg],"afp")==0) mcrible++; else
			if (stricmp(myreg[maxreg],"bcp")==0) mcrible++; else
			if (stricmp(myreg[maxreg],"dep")==0) mcrible++; else
			if (stricmp(myreg[maxreg],"hlp")==0) mcrible++;
		}
		initialize_crible(param->maxRun);
	}

	printf("Configure Emulation\n");
	// reset emulator + memory preparation
	z80_init(&z);
	if (param->haltStop) z.breakOnHalt=1;
	if (param->eiStop) z.breakOnEI=1;
	if (param->unknownStop) z.breakOnUnknownInstruction=1;
	if (param->ioCPC) {
		z.port_in=cpc_wrapin;
		z.port_out=cpc_wrapout;
	}
	if (param->addVec38) {
		memory[0x38]=0xFB;
		memory[0x39]=0xC9;
	}
	// copy the whole 64K (dirt + programm + vector) into the emulated RAM
	memcpy(&z.userdata[0],memory,65536);

	printf("Start Emulation\n");
	if (!param->maxRun) param->maxRun=1; // execute at least once
					     //
	for (i=0;i<param->maxRun;i++) {
		z.pc=param->runAt;
		z.breakSuccess=0;
		z.breakReason=Z80_BREAK_NONE;

		// reset some CPU states
		  z.iff_delay = 0;
		  z.interrupt_mode = 0;
		  z.iff1 = 0;
		  z.iff2 = 0;
		  z.halted = 0;
		  z.int_pending = 0;
		  z.nmi_pending = 0;
		  z.int_data = 0;
		// reset stats
		z.nbinstructions=0;
		z.nop=z.cyc=0;

		if (param->a!=-1)    z.a=param->a;
		if (param->regF!=-1) z80_set_f(&z,param->regF);
		if (param->b!=-1)    z.b=param->b;
		if (param->c!=-1)    z.c=param->c;
		if (param->d!=-1)    z.d=param->d;
		if (param->e!=-1)    z.e=param->e;
		if (param->h!=-1)    z.h=param->h;
		if (param->l!=-1)    z.l=param->l;
		if (param->xh!=-1)   z.ix=(z.ix&0x00FF)|((param->xh&0xFF)<<8);
		if (param->xl!=-1)   z.ix=(z.ix&0xFF00)|(param->xl&0xFF);
		if (param->yh!=-1)   z.iy=(z.iy&0x00FF)|((param->yh&0xFF)<<8);
		if (param->yl!=-1)   z.iy=(z.iy&0xFF00)|(param->yl&0xFF);
		if (param->sp!=-1)   z.sp=param->sp;
		if (param->af!=-1)   {unsigned char oldf=z.f_;z.a=param->af>>8;z.f_=param->af&0xFF;z80_set_f(&z,z.f_);z.f_=oldf;}
		if (param->bc!=-1)   {z.b=param->bc>>8;z.c=param->bc&0xFF;}
		if (param->de!=-1)   {z.d=param->de>>8;z.e=param->de&0xFF;}
		if (param->hl!=-1)   {z.h=param->hl>>8;z.l=param->hl&0xFF;}
		if (param->ix!=-1)   z.ix=param->ix;
		if (param->iy!=-1)   z.iy=param->iy;
		// EXX
		if (param->ap!=-1)  z.a_=param->ap;
		if (param->fp!=-1)  z.f_=param->fp;
		if (param->bp!=-1)  z.b_=param->bp;
		if (param->cp!=-1)  z.c_=param->cp;
		if (param->dp!=-1)  z.d_=param->dp;
		if (param->ep!=-1)  z.e_=param->ep;
		if (param->hp!=-1)  z.h_=param->hp;
		if (param->lp!=-1)  z.l_=param->lp;
		if (param->afp!=-1) {z.a_=param->afp>>8;z.f_=param->afp&0xFF;}
		if (param->bcp!=-1) {z.b_=param->bcp>>8;z.c_=param->bcp&0xFF;}
		if (param->dep!=-1) {z.d_=param->dep>>8;z.e_=param->dep&0xFF;}
		if (param->hlp!=-1) {z.h_=param->hlp>>8;z.l_=param->hlp&0xFF;}

		if (param->crible) {
			for (ireg=0;myreg[ireg];ireg++) {
				if (stricmp(myreg[ireg],"a")==0) z.a=crible8(); else
				if (stricmp(myreg[ireg],"f")==0) z80_set_f(&z,crible8()); else
				if (stricmp(myreg[ireg],"b")==0) z.b=crible8(); else
				if (stricmp(myreg[ireg],"c")==0) z.c=crible8(); else
				if (stricmp(myreg[ireg],"d")==0) z.d=crible8(); else
				if (stricmp(myreg[ireg],"e")==0) z.e=crible8(); else
				if (stricmp(myreg[ireg],"h")==0) z.h=crible8(); else
				if (stricmp(myreg[ireg],"l")==0) z.l=crible8(); else
				if (stricmp(myreg[ireg],"xh")==0) z.ix=(z.ix&0xFF)|(crible8()<<8); else
				if (stricmp(myreg[ireg],"xl")==0) z.ix=(z.ix&0xFF00)|crible8(); else
				if (stricmp(myreg[ireg],"yh")==0) z.iy=(z.iy&0xFF)|(crible8()<<8); else
				if (stricmp(myreg[ireg],"yl")==0) z.iy=(z.iy&0xFF00)|crible8(); else
				if (stricmp(myreg[ireg],"af")==0) {z.a=crible8();z80_set_f(&z,crible8());} else
				if (stricmp(myreg[ireg],"bc")==0) {z.b=crible8();z.c=crible8();} else
				if (stricmp(myreg[ireg],"de")==0) {z.d=crible8();z.e=crible8();} else
				if (stricmp(myreg[ireg],"hl")==0) {z.h=crible8();z.l=crible8();} else
				if (stricmp(myreg[ireg],"ix")==0) z.ix=crible16(); else
				if (stricmp(myreg[ireg],"iy")==0) z.iy=crible16(); else
				if (stricmp(myreg[ireg],"ap")==0) z.a_=crible8(); else
				if (stricmp(myreg[ireg],"fp")==0) z.f_=crible8(); else
				if (stricmp(myreg[ireg],"bp")==0) z.b_=crible8(); else
				if (stricmp(myreg[ireg],"cp")==0) z.c_=crible8(); else
				if (stricmp(myreg[ireg],"dp")==0) z.d_=crible8(); else
				if (stricmp(myreg[ireg],"ep")==0) z.e_=crible8(); else
				if (stricmp(myreg[ireg],"hp")==0) z.h_=crible8(); else
				if (stricmp(myreg[ireg],"lp")==0) z.l_=crible8(); else
				if (stricmp(myreg[ireg],"afp")==0) {z.a_=crible8();z.f_=crible8();} else
				if (stricmp(myreg[ireg],"bcp")==0) {z.b_=crible8();z.c_=crible8();} else
				if (stricmp(myreg[ireg],"dep")==0) {z.d_=crible8();z.e_=crible8();} else
				if (stricmp(myreg[ireg],"hlp")==0) {z.h_=crible8();z.l_=crible8();} else
				{
					fprintf(stderr,"unknown register to crible [%s]\n",myreg[ireg]);
					exit(2);
				}
			}
			crible_next_run();
		}

		CaptureRegs(&z,&inputRegs); // what this RUN starts with (crible values included)

		// RUN
		while (1) {
		    if (param->debug) z80_debug_output(&z);
		    instrPC=z.pc;
		    z80_step(&z);
		    z.nbinstructions++;
		    if (z.breakSuccess) {
			    switch (z.breakReason) {
				    case Z80_BREAK_EI:      stopReason=STOP_EI; break;
				    case Z80_BREAK_UNKNOWN: stopReason=STOP_UNKNOWN; break;
				    default:                stopReason=STOP_HALT; break;
			    }
			    break;
		    }
		    if (z.pc==param->stopAddress) { stopReason=STOP_ADDRESS; break; }
		    // execution limits
		    if (param->useMaxNop && z.nop>=param->maxNop)       { stopReason=STOP_MAXNOP; break; }
		    if (param->useMaxCycle && z.cyc>=param->maxCycle)   { stopReason=STOP_MAXCYCLE; break; }

		    if (param->genINT) {
			    INTcpt++;
			    if (INTcpt>=INTcptMax) {
				    INTcpt=0;
				    z80_gen_int(&z,0xFF);
			    }
		    }
		    if (param->genVBL) {
			    VBLcpt++;
			    if (VBLcpt>=VBLcptMax) {
				    hasVBL=512+z.nop; // approx 512 nops
			    }
		    }
		}

		CaptureRegs(&z,&outputRegs);

		// a RUN fails when it does not end normally or does not give the expected result
		if (stopReason==STOP_MAXNOP || stopReason==STOP_MAXCYCLE) exitCode=EXIT_RUN_LIMIT; else
		if (stopReason==STOP_UNKNOWN) exitCode=EXIT_RUN_UNKNOWN; else
		if (CheckExpectations(param->check,&outputRegs,z.userdata,0)) exitCode=EXIT_RUN_EXPECT;

		if (maxRunDisplay<10) printf("Executed in %ld nop(s) (cycles=%ld) MIPS=%.2lf cycleMIPS=%.2lf (@4MHz)\n",z.nop,z.cyc,(double)z.nbinstructions/(double)z.nop,(double)z.nbinstructions/(double)z.cyc*4.0);
		maxRunDisplay++;

		totalNop+=z.nop;
		totalCyc+=z.cyc;
		totalRun++;
		if (!i) {
			minRun=maxRun=z.nop;
			minRunC=maxRunC=z.cyc;
		} else {
			if (minRun>z.nop) minRun=z.nop;
			if (maxRun<z.nop) maxRun=z.nop;
			if (minRunC>z.cyc) minRunC=z.cyc;
			if (maxRunC<z.cyc) maxRunC=z.cyc;
		}
		if (exitCode!=EXIT_RUN_OK) break; // keep the failing RUN for the report
	}
	if (totalRun>1) {
		printf("Executed %lld times\n",totalRun);
		printf("Average RUN : %lld nops   %lld cycles\n",totalNop/totalRun, totalCyc/totalRun);
		printf("Minimum RUN : %ld nops   %ld cycles\n",minRun,minRunC);
		printf("Maximum RUN : %ld nops   %ld cycles\n",maxRun,maxRunC);
	}

	for (i=0;i<65536;i++) {
		switch (z.userCond[i]) {
			case 0:break;
			case 1:
			case 2:JPCondPartial++;
			case 3:JPCond++;break;
			case 4:
			case 8:CALLCondPartial++;
			case 12:CALLCond++;break;
			case 16:
			case 32:RETCondPartial++;
			case 48:RETCond++;break;
			case 64:
			case 128:JRCondPartial++;
			case 192:JRCond++;break;
			default:printf("Internal error with condition stats v=#%02X (%d)\n",z.userCond[i],z.userCond[i]);exit(2);break;
		}
	}
	totalCond=JRCond+RETCond+CALLCond+JPCond;
	if (totalCond) {
		printf("Branch coverage :\n");
		printf("Conditionnal CALL : %d ",CALLCond); if (CALLCond) printf("coverage %.2lf%%",100.0-CALLCondPartial*50.0/CALLCond); printf("\n");
		printf("Conditionnal JUMP : %d ",JPCond); if (JPCond) printf("coverage %.2lf%%",100.0-JPCondPartial*50.0/JPCond); printf("\n");
		printf("Conditionnal JR   : %d ",JRCond); if (JRCond) printf("coverage %.2lf%%",100.0-JRCondPartial*50.0/JRCond); printf("\n");
		printf("Conditionnal RET  : %d ",RETCond); if (RETCond) printf("coverage %.2lf%%",100.0-RETCondPartial*50.0/RETCond); printf("\n");
	}
	for (i=param->destOffset;i<param->destOffset+zesize;i++) {
		if (z.userRW[i]) rawCoverage++;
	}
	printf("RAW memory coverage : %.2lf%%\n",rawCoverage*100.0/zesize);

	// final report, about the last RUN (or the first failing one)
	PrintStop(stopReason,&z,instrPC);
	if (param->maxRun>1) {
		printf("RUN: %lld/%u%s\n",totalRun,param->maxRun,exitCode!=EXIT_RUN_OK?" (stopped on failure)":"");
		PrintRegs("INPUT:","INALT:",&inputRegs); // what made this RUN (crible values)
	}
	PrintRegs("REGS:","ALT :",&outputRegs);
	if (param->check->nreg || param->check->nmem) {
		if (CheckExpectations(param->check,&outputRegs,z.userdata,1)==0) printf("EXPECT OK\n");
	}
	DoDumps(param->check,z.userdata);
	printf("EXIT: %d\n",exitCode);
	return exitCode;
}

void Usage(int errcode) {
	printf("===========   Z80 profiler   ===========\n");
	printf("Emulation based on Nicolas Allemand work\n");
	printf("\n");
	printf("syntax : emuZ80 <filename> <options>\n");
	printf("\n");
	printf("load options :\n");
	printf("-fileOffset <value>         to skip first bytes\n");
	printf("-fileLength <value>         to truncate reading\n");
	printf("-destOffset <value>         read at memory address\n");
	printf("\n");
	printf("emulation options :\n");
	printf("-VBL                        generate VBL signal\n");
	printf("-VBLdelay <value>           delay between each VBL\n");
	printf("-INT                        generate INT signal\n");
	printf("-INTdelay <value>[,<value>] delay(s) between each INT\n");
	printf("-dirt 0|1|2                 0: use clean memory\n");
	printf("                            1: use CPC like memory\n");
	printf("                            2: use shuffled memory\n");
	printf("-IOCPC                      mimic CPC input/output\n");
	printf("\n");
	printf("emulation init options :\n");
	printf("-pc <value>                 set PC value when starting\n");
	printf("                            ex: -pc 0x100\n");
	printf("-<reg> <value>              set register value\n");
	printf("                            ex: -bc 0x100\n");
	printf("     reg may be : a,f,b,c,d,e,h,l,xl,xh,yl,yh\n");
	printf("                  af,bc,de,hl,ix,iy\n");
	printf("                  afp,bcp,dep,hlp,ap,fp,bp,cp,dp,ep,hp,lp\n");
	printf("-crible <reg>,<reg>,...     crible registers in the list\n");
	printf("-addVec38                   add EI:RET in #38\n");
	printf("\n");
	printf("run options :\n");
	printf("-maxnop <value>             RUN maximum duration in nops\n");
	printf("-maxCycle <value>           RUN maximum duration in cycles\n");
	printf("-maxRun <value>             RUN multiple times\n");
	printf("-stopAddress <value>        RUN until reaching address (disabled by default)\n");
	printf("-eiStop                     RUN until EI\n");
	printf("-haltStop                   RUN until HALT\n");
	printf("-unknownStop                RUN until unknown instruction\n");
	printf("-debug                      ultra verbose information (for debug purpose)\n");
	printf("\n");
	printf("result options (values: decimal, 0x1234 or #1234) :\n");
	printf("-expect <reg>=<value>       check a register when RUN ends (repeatable)\n");
	printf("                            ex: -expect hl=#1234 -expect a=0\n");
	printf("-expectMem <addr>,<b>[,<b>] check consecutive bytes when RUN ends (repeatable)\n");
	printf("-dump <addr>,<length>       hexa dump of memory when RUN ends (repeatable)\n");
	printf("-dumpFile <addr>,<len>,<f>  write memory to a binary file when RUN ends\n");
	printf("\n");
	printf("report (end of output, one tag per line) :\n");
	printf("STOP: <reason>              HALT, EI, UNKNOWN, STOPADDRESS, MAXNOP, MAXCYCLE\n");
	printf("RUN: / INPUT: / INALT:      with -maxRun, run count and starting registers\n");
	printf("REGS: / ALT :               registers when RUN ends, flags as SZ5H3VNC\n");
	printf("EXPECT OK | EXPECT FAIL:    result of -expect / -expectMem\n");
	printf("MEM #xxxx: / DUMP           result of -dump / -dumpFile\n");
	printf("EXIT: <code>                also the process exit code\n");
	printf("\n");
	printf("exit codes : 0 ok, 1 bad parameter, 2 file error, 3 nop/cycle limit reached,\n");
	printf("             4 unknown instruction, 5 expectation failed\n");
	printf("with -maxRun, runs stop at the first failing one\n");
	exit(errcode);
}

int ParseOptions(char **argv,int argc, struct s_parameter *param)
{
	#undef FUNC
	#define FUNC "ParseOptions"
	
	char *sep;
	int i=0;

	if (argv[0][0]=='-') {
		if (stricmp(argv[i],"-crible")==0) {
			if (i+1<argc) {
				param->crible=argv[1];
				printf("crible setting [%s]\n",param->crible);
				i++;
			} else {
				fprintf(stderr,"ERROR : Missing argument for -crible option\nRun without option for help\n");
				exit(1);
			}
		} else if (stricmp(argv[i],"-fileOffset")==0) {
			if (i+1<argc) {
				if (argv[1][0]=='0' && argv[1][1]=='x') param->fileOffset=strtol(argv[1],NULL,16); else
				param->fileOffset=atoi(argv[1]);
				printf("fileOffset setting #%04X (%d)\n",param->fileOffset,param->fileOffset);
				i++;
			} else {
				fprintf(stderr,"ERROR : Missing argument for -fileOffset option\nRun without option for help\n");
				exit(1);
			}
		} else if (stricmp(argv[i],"-fileLength")==0) {
			if (i+1<argc) {
				if (argv[1][0]=='0' && argv[1][1]=='x') param->fileLength=strtol(argv[1],NULL,16); else
				param->fileLength=atoi(argv[1]);
				printf("fileLength setting #%04X (%d)\n",param->fileLength,param->fileLength);
				i++;
			} else {
				fprintf(stderr,"ERROR : Missing argument for -fileLength option\nRun without option for help\n");
				exit(1);
			}
		} else if (stricmp(argv[i],"-destOffset")==0) {
			if (i+1<argc) {
				if (argv[1][0]=='0' && argv[1][1]=='x') param->destOffset=strtol(argv[1],NULL,16); else
				param->destOffset=atoi(argv[1]);
				printf("destOffset setting #%04X (%d)\n",param->destOffset,param->destOffset);
				i++;
			} else {
				fprintf(stderr,"ERROR : Missing argument for -destOffset option\nRun without option for help\n");
				exit(1);
			}
		} else if (stricmp(argv[i],"-stopAddress")==0) {
			if (i+1<argc) {
				if (argv[1][0]=='0' && argv[1][1]=='x') param->stopAddress=strtol(argv[1],NULL,16); else
				param->stopAddress=atoi(argv[1]);
				printf("stopAddress setting #%04X (%d)\n",param->stopAddress,param->stopAddress);
				i++;
			} else {
				fprintf(stderr,"ERROR : Missing argument for -stopAddress option\nRun without option for help\n");
				exit(1);
			}
		} else if (stricmp(argv[i],"-maxrun")==0) {
			if (i+1<argc) {
				if (argv[1][0]=='0' && argv[1][1]=='x') param->maxRun=strtol(argv[1],NULL,16); else
				param->maxRun=atoi(argv[1]);
				printf("maxRun setting #%04X (%d)\n",param->maxRun,param->maxRun);
				i++;
			} else {
				fprintf(stderr,"ERROR : Missing argument for -maxRun option\nRun without option for help\n");
				exit(1);
			}
		} else if (stricmp(argv[i],"-maxnop")==0) {
			if (i+1<argc) {
				param->maxNop=strtoul(argv[1],NULL,0); // decimal or 0x hexa
				param->useMaxNop=1;
				printf("maxNop setting %u\n",param->maxNop);
				i++;
			} else {
				fprintf(stderr,"ERROR : Missing argument for -maxnop option\nRun without option for help\n");
				exit(1);
			}
		} else if (stricmp(argv[i],"-maxCycle")==0) {
			if (i+1<argc) {
				param->maxCycle=strtoul(argv[1],NULL,0); // decimal or 0x hexa
				param->useMaxCycle=1;
				printf("maxCycle setting %u\n",param->maxCycle);
				i++;
			} else {
				fprintf(stderr,"ERROR : Missing argument for -maxCycle option\nRun without option for help\n");
				exit(1);
			}
		} else if (stricmp(argv[i],"-expect")==0) {
			if (i+1<argc) { AddExpectReg(param->check,argv[1]); i++; } else {
			fprintf(stderr,"ERROR : Missing argument for -expect option\nRun without option for help\n"); exit(1); }
		} else if (stricmp(argv[i],"-expectMem")==0) {
			if (i+1<argc) { AddExpectMem(param->check,argv[1]); i++; } else {
			fprintf(stderr,"ERROR : Missing argument for -expectMem option\nRun without option for help\n"); exit(1); }
		} else if (stricmp(argv[i],"-dump")==0) {
			if (i+1<argc) { AddDump(param->check,argv[1],0); i++; } else {
			fprintf(stderr,"ERROR : Missing argument for -dump option\nRun without option for help\n"); exit(1); }
		} else if (stricmp(argv[i],"-dumpFile")==0) {
			if (i+1<argc) { AddDump(param->check,argv[1],1); i++; } else {
			fprintf(stderr,"ERROR : Missing argument for -dumpFile option\nRun without option for help\n"); exit(1); }
		} else if (stricmp(argv[i],"-INTdelay")==0) {
			if (i+1<argc) {
				if (argv[1][0]=='0' && argv[1][1]=='x') param->genINTdelay=strtol(argv[1],NULL,16); else
				param->genINTdelay=atoi(argv[1]);
				printf("INT delay setting #%04X (%d)\n",param->genINTdelay,param->genINTdelay);
			} else {
				fprintf(stderr,"ERROR : Missing argument for -VBLdelay option\nRun without option for help\n");
				exit(1);
			}
		} else if (stricmp(argv[i],"-VBLdelay")==0) {
			if (i+1<argc) {
				if (argv[1][0]=='0' && argv[1][1]=='x') param->genVBLdelay=strtol(argv[1],NULL,16); else
				param->genVBLdelay=atoi(argv[1]);
				printf("VBL delay setting #%04X (%d)\n",param->genVBLdelay,param->genVBLdelay);
			} else {
				fprintf(stderr,"ERROR : Missing argument for -VBLdelay option\nRun without option for help\n");
				exit(1);
			}
		} else if (stricmp(argv[i],"-INT")==0) {
			param->genINT=1;
		} else if (stricmp(argv[i],"-VBL")==0) {
			param->genVBL=1;
		} else if (stricmp(argv[i],"-debug")==0) {
			param->debug=1;
		} else if (stricmp(argv[i],"-addVec38")==0) {
			param->addVec38=1;
		} else if (stricmp(argv[i],"-IOCPC")==0) {
			param->ioCPC=1;
		} else if (stricmp(argv[i],"-unknownStop")==0) {
			param->unknownStop=1;
		} else if (stricmp(argv[i],"-eiStop")==0) {
			param->eiStop=1;
		} else if (stricmp(argv[i],"-haltStop")==0) {
			param->haltStop=1;
		} else if (stricmp(argv[i],"-dirt")==0) {
			if (i+1<argc) {
				i++;
				if (strcmp(argv[1],"0")==0) {}
				else if (strcmp(argv[1],"1")==0) param->dirtMemory=1;
				else if (strcmp(argv[1],"2")==0) param->dirtMemory=2;
				else {
					fprintf(stderr,"ERROR : Invalid argument for -dirt option\nRun without option for help\n");
					exit(1);
				}
			} else {
				fprintf(stderr,"ERROR : Missing argument for -dirt option\nRun without option for help\n");
				exit(1);
			}
		} else if (stricmp(argv[i],"-pc")==0) {
			if (i+1<argc) {
				if (argv[1][0]=='0' && argv[1][1]=='x') param->runAt=strtol(argv[1],NULL,16); else
				param->runAt=atoi(argv[1]);
				printf("will RUN at #%04X (%d)\n",param->runAt,param->runAt);
				i++;
			} else {
				fprintf(stderr,"ERROR : Missing argument for -pc option\nRun without option for help\n");
				exit(1);
			}
		} else if (stricmp(argv[i],"-a")==0) {
			if (i+1<argc) { if (argv[1][0]=='0' && argv[1][1]=='x') param->a=strtol(argv[1],NULL,16); else param->a=atoi(argv[1]); i++; } else {
			fprintf(stderr,"ERROR : Missing argument for -a option\nRun without option for help\n"); exit(1); }  
		} else if (stricmp(argv[i],"-b")==0) {
			if (i+1<argc) { if (argv[1][0]=='0' && argv[1][1]=='x') param->b=strtol(argv[1],NULL,16); else param->b=atoi(argv[1]); i++; } else {
			fprintf(stderr,"ERROR : Missing argument for -b option\nRun without option for help\n"); exit(1); }  
		} else if (stricmp(argv[i],"-c")==0) {
			if (i+1<argc) { if (argv[1][0]=='0' && argv[1][1]=='x') param->c=strtol(argv[1],NULL,16); else param->c=atoi(argv[1]); i++; } else {
			fprintf(stderr,"ERROR : Missing argument for -c option\nRun without option for help\n"); exit(1); }  
		} else if (stricmp(argv[i],"-d")==0) {
			if (i+1<argc) { if (argv[1][0]=='0' && argv[1][1]=='x') param->d=strtol(argv[1],NULL,16); else param->d=atoi(argv[1]); i++; } else {
			fprintf(stderr,"ERROR : Missing argument for -d option\nRun without option for help\n"); exit(1); }  
		} else if (stricmp(argv[i],"-e")==0) {
			if (i+1<argc) { if (argv[1][0]=='0' && argv[1][1]=='x') param->e=strtol(argv[1],NULL,16); else param->e=atoi(argv[1]); i++; } else {
			fprintf(stderr,"ERROR : Missing argument for -e option\nRun without option for help\n"); exit(1); }  
		} else if (stricmp(argv[i],"-f")==0) {
			if (i+1<argc) { if (argv[1][0]=='0' && argv[1][1]=='x') param->regF=strtol(argv[1],NULL,16); else param->regF=atoi(argv[1]); i++; } else {
			fprintf(stderr,"ERROR : Missing argument for -f option\nRun without option for help\n"); exit(1); }  
		} else if (stricmp(argv[i],"-h")==0) {
			if (i+1<argc) { if (argv[1][0]=='0' && argv[1][1]=='x') param->h=strtol(argv[1],NULL,16); else param->h=atoi(argv[1]); i++; } else {
			fprintf(stderr,"ERROR : Missing argument for -h option\nRun without option for help\n"); exit(1); }  
		} else if (stricmp(argv[i],"-l")==0) {
			if (i+1<argc) { if (argv[1][0]=='0' && argv[1][1]=='x') param->l=strtol(argv[1],NULL,16); else param->l=atoi(argv[1]); i++; } else {
			fprintf(stderr,"ERROR : Missing argument for -l option\nRun without option for help\n"); exit(1); }  
		} else if (stricmp(argv[i],"-xl")==0) {
			if (i+1<argc) { if (argv[1][0]=='0' && argv[1][1]=='x') param->xl=strtol(argv[1],NULL,16); else param->xl=atoi(argv[1]); i++; } else {
			fprintf(stderr,"ERROR : Missing argument for -xl option\nRun without option for help\n"); exit(1); } 
		} else if (stricmp(argv[i],"-xh")==0) {
			if (i+1<argc) { if (argv[1][0]=='0' && argv[1][1]=='x') param->xh=strtol(argv[1],NULL,16); else param->xh=atoi(argv[1]); i++; } else {
			fprintf(stderr,"ERROR : Missing argument for -xh option\nRun without option for help\n"); exit(1); }  
		} else if (stricmp(argv[i],"-yl")==0) {
			if (i+1<argc) { if (argv[1][0]=='0' && argv[1][1]=='x') param->yl=strtol(argv[1],NULL,16); else param->yl=atoi(argv[1]); i++; } else {
			fprintf(stderr,"ERROR : Missing argument for -yl option\nRun without option for help\n"); exit(1); }  
		} else if (stricmp(argv[i],"-yh")==0) {
			if (i+1<argc) { if (argv[1][0]=='0' && argv[1][1]=='x') param->yh=strtol(argv[1],NULL,16); else param->yh=atoi(argv[1]); i++; } else {
			fprintf(stderr,"ERROR : Missing argument for -yh option\nRun without option for help\n"); exit(1); }
		} else if (stricmp(argv[i],"-ix")==0) {
			if (i+1<argc) { if (argv[1][0]=='0' && argv[1][1]=='x') param->ix=strtol(argv[1],NULL,16); else param->ix=atoi(argv[1]); i++; } else {
			fprintf(stderr,"ERROR : Missing argument for -ix option\nRun without option for help\n"); exit(1); }
		} else if (stricmp(argv[i],"-iy")==0) {
			if (i+1<argc) { if (argv[1][0]=='0' && argv[1][1]=='x') param->iy=strtol(argv[1],NULL,16); else param->iy=atoi(argv[1]); i++; } else {
			fprintf(stderr,"ERROR : Missing argument for -iy option\nRun without option for help\n"); exit(1); }

		} else if (stricmp(argv[i],"-af")==0) {
			if (i+1<argc) { if (argv[1][0]=='0' && argv[1][1]=='x') param->af=strtol(argv[1],NULL,16); else param->af=atoi(argv[1]); i++; } else {
			fprintf(stderr,"ERROR : Missing argument for -af option\nRun without option for help\n"); exit(1); }
		} else if (stricmp(argv[i],"-bc")==0) {
			if (i+1<argc) { if (argv[1][0]=='0' && argv[1][1]=='x') param->bc=strtol(argv[1],NULL,16); else param->bc=atoi(argv[1]); i++; } else {
			fprintf(stderr,"ERROR : Missing argument for -bc option\nRun without option for help\n"); exit(1); }
		} else if (stricmp(argv[i],"-de")==0) {
			if (i+1<argc) { if (argv[1][0]=='0' && argv[1][1]=='x') param->de=strtol(argv[1],NULL,16); else param->de=atoi(argv[1]); i++; } else {
			fprintf(stderr,"ERROR : Missing argument for -de option\nRun without option for help\n"); exit(1); }
		} else if (stricmp(argv[i],"-hl")==0) {
			if (i+1<argc) { if (argv[1][0]=='0' && argv[1][1]=='x') param->hl=strtol(argv[1],NULL,16); else param->hl=atoi(argv[1]); i++; } else {
			fprintf(stderr,"ERROR : Missing argument for -hl option\nRun without option for help\n"); exit(1); }
		} else if (stricmp(argv[i],"-sp")==0) {
			if (i+1<argc) { if (argv[1][0]=='0' && argv[1][1]=='x') param->sp=strtol(argv[1],NULL,16); else param->sp=atoi(argv[1]); i++; } else {
			fprintf(stderr,"ERROR : Missing argument for -sp option\nRun without option for help\n"); exit(1); }
		// extended registers
		} else if (stricmp(argv[i],"-ap")==0) {
			if (i+1<argc) { if (argv[1][0]=='0' && argv[1][1]=='x') param->ap=strtol(argv[1],NULL,16); else param->ap=atoi(argv[1]); i++; } else {
			fprintf(stderr,"ERROR : Missing argument for -ap option\nRun without option for help\n"); exit(1); }  
		} else if (stricmp(argv[i],"-bp")==0) {
			if (i+1<argc) { if (argv[1][0]=='0' && argv[1][1]=='x') param->bp=strtol(argv[1],NULL,16); else param->bp=atoi(argv[1]); i++; } else {
			fprintf(stderr,"ERROR : Missing argument for -bp option\nRun without option for help\n"); exit(1); }  
		} else if (stricmp(argv[i],"-cp")==0) {
			if (i+1<argc) { if (argv[1][0]=='0' && argv[1][1]=='x') param->cp=strtol(argv[1],NULL,16); else param->cp=atoi(argv[1]); i++; } else {
			fprintf(stderr,"ERROR : Missing argument for -cp option\nRun without option for help\n"); exit(1); }  
		} else if (stricmp(argv[i],"-dp")==0) {
			if (i+1<argc) { if (argv[1][0]=='0' && argv[1][1]=='x') param->dp=strtol(argv[1],NULL,16); else param->dp=atoi(argv[1]); i++; } else {
			fprintf(stderr,"ERROR : Missing argument for -dp option\nRun without option for help\n"); exit(1); }  
		} else if (stricmp(argv[i],"-ep")==0) {
			if (i+1<argc) { if (argv[1][0]=='0' && argv[1][1]=='x') param->ep=strtol(argv[1],NULL,16); else param->ep=atoi(argv[1]); i++; } else {
			fprintf(stderr,"ERROR : Missing argument for -ep option\nRun without option for help\n"); exit(1); }  
		} else if (stricmp(argv[i],"-fp")==0) {
			if (i+1<argc) { if (argv[1][0]=='0' && argv[1][1]=='x') param->fp=strtol(argv[1],NULL,16); else param->fp=atoi(argv[1]); i++; } else {
			fprintf(stderr,"ERROR : Missing argument for -fp option\nRun without option for help\n"); exit(1); }  
		} else if (stricmp(argv[i],"-hp")==0) {
			if (i+1<argc) { if (argv[1][0]=='0' && argv[1][1]=='x') param->hp=strtol(argv[1],NULL,16); else param->hp=atoi(argv[1]); i++; } else {
			fprintf(stderr,"ERROR : Missing argument for -hp option\nRun without option for help\n"); exit(1); }  
		} else if (stricmp(argv[i],"-lp")==0) {
			if (i+1<argc) { if (argv[1][0]=='0' && argv[1][1]=='x') param->lp=strtol(argv[1],NULL,16); else param->lp=atoi(argv[1]); i++; } else {
			fprintf(stderr,"ERROR : Missing argument for -lp option\nRun without option for help\n"); exit(1); }  
		} else if (stricmp(argv[i],"-afp")==0) {
			if (i+1<argc) { if (argv[1][0]=='0' && argv[1][1]=='x') param->afp=strtol(argv[1],NULL,16); else param->afp=atoi(argv[1]); i++; } else {
			fprintf(stderr,"ERROR : Missing argument for -afp option\nRun without option for help\n"); exit(1); }
		} else if (stricmp(argv[i],"-bcp")==0) {
			if (i+1<argc) { if (argv[1][0]=='0' && argv[1][1]=='x') param->bcp=strtol(argv[1],NULL,16); else param->bcp=atoi(argv[1]); i++; } else {
			fprintf(stderr,"ERROR : Missing argument for -bcp option\nRun without option for help\n"); exit(1); }
		} else if (stricmp(argv[i],"-dep")==0) {
			if (i+1<argc) { if (argv[1][0]=='0' && argv[1][1]=='x') param->dep=strtol(argv[1],NULL,16); else param->dep=atoi(argv[1]); i++; } else {
			fprintf(stderr,"ERROR : Missing argument for -dep option\nRun without option for help\n"); exit(1); }
		} else if (stricmp(argv[i],"-hlp")==0) {
			if (i+1<argc) { if (argv[1][0]=='0' && argv[1][1]=='x') param->hlp=strtol(argv[1],NULL,16); else param->hlp=atoi(argv[1]); i++; } else {
			fprintf(stderr,"ERROR : Missing argument for -hlp option\nRun without option for help\n"); exit(1); }

		} else { fprintf(stderr,"ERROR : Invalid option [%s]\nRun without option for help\n",argv[0]); exit(1); }
	} else if (!param->filename) {
		// open file
		param->f=fopen(argv[0],"rb");
		if (param->f) {
#ifdef OS_WIN
int sr;
sr=_setmode(_fileno(param->f), _O_BINARY );
if (sr==-1) {
fprintf(stderr,"FATAL: cannot set binary mode for reading");
exit(1);
}
#endif

			param->filename=argv[0];
			printf("opening [%s] as binary data\n",param->filename);
		} else {
			fprintf(stderr,"ERROR : Cannot open [%s] for reading\n",argv[0]);
			exit(1);
		}
	} else {
		fprintf(stderr,"ERROR : Already have a filename. What is [%s] parameter supposed to do?\nRun without option for help\n",argv[0]);
		exit(1);
	}

	return i;
}

void GetParametersFromCommandLine(int argc, char **argv, struct s_parameter *param) {
	int i;

	if (argc<2) {
		Usage(0); // no option: display help
	}
	for (i=1;i<argc;i++)
		i+=ParseOptions(&argv[i],argc-i,param);

	if (!param->filename) {
		fprintf(stderr,"ERROR : You need at least a <filename> to inject data in Z80 emulation\n");
		Usage(1);
	}
}

void main(int argc, char **argv) {
	static struct s_check check={0}; // static: too big for the stack
	struct s_parameter param={0};
	param.check=&check;
	param.stopAddress=0x12345;
	param.a=-1; param.regF=-1; param.b=-1; param.c=-1; param.d=-1; param.e=-1;
	param.h=-1; param.l=-1; param.xh=-1; param.xl=-1; param.yh=-1; param.yl=-1;
	param.sp=-1; param.af=-1; param.bc=-1; param.de=-1; param.hl=-1; param.ix=-1;
	param.iy=-1; param.ap=-1; param.fp=-1; param.bp=-1; param.cp=-1; param.dp=-1;
	param.ep=-1; param.hp=-1; param.lp=-1; param.afp=-1; param.bcp=-1; param.dep=-1;
	param.hlp=-1;

	GetParametersFromCommandLine(argc,argv,&param);
	exit(EmuZ80(&param));
}

