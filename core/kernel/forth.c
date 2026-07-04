#include <core/kernel/shell.h>
#include <core/kernel/kstd.h>
#include <core/drivers/keyboard.h>
#include <core/kernel/vge/fb.h>
#include <core/kernel/mem.h>
#include <core/fs/iso9660.h>
#include <core/fs/vfs.h>
#include <core/kernel/nvm/nvm.h>
#include <core/kernel/nvm/caps.h>
#include <log.h>
#include <core/arch/work_queue.h>
#include <core/arch/smp.h>
#include <core/kernel/mem/slab.h>
#include <core/kernel/tty.h>
#include <core/fs/procfs.h>
#include <core/arch/io.h>
#include <core/kernel/vge/palette.h>

#define KEYBOARD_DATA_PORT    0x60
#define KEYBOARD_STATUS_PORT  0x64

#define MAX_PATH_LENGTH 256
static char current_working_directory[MAX_PATH_LENGTH] = "/";
vfs_dirent_t* entries = NULL;

char* input_buffer[256];

#define DSTACK_SIZE 1000 /* cells reserved for the stack */
#define RSTACK_SIZE 1000 /* cells reserved for the return stack */
#define HERE_SIZE1 0x6000

typedef uint64_t Cell;
typedef int64_t sCell;
typedef  void (*proc)(void);

#define pp(cName) const sCell p##cName = (sCell)cName;

#define SIGN_PRIM_MASK 0xffffffff00000000

static int forth_run;
sCell * HereArea;
sCell * StackArea;
sCell * RStackArea;
sCell * here;
sCell *Stack;
sCell *rStack;
sCell * ip ;
sCell ireg;
sCell Tos;
sCell * Handler = NULL;

Cell numericBase = 10;

Cell SignPrim;

Cell text_color = 15;


static void serial_hex64(uint64_t v) {
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 15; i >= 0; i--) {
        uint8_t nib = (v >> (i * 4)) & 0xF;
        buf[17 - i] = (char)(nib < 10 ? '0' + nib : 'a' + nib - 10);
    }
    buf[18] = '\0';
    serial_print(buf);
}

void Co( sCell cod ){ *here++ =  cod ; }
void Co2( sCell cod1,sCell cod2 ){Co(cod1); Co(cod2); }
void Co3( sCell cod1,sCell cod2,sCell cod3 ){Co2(cod1,cod2); ; Co(cod3); }
void Co4( sCell cod1,sCell cod2,sCell cod3,sCell cod4 ){  Co3(cod1,cod2,cod3); ; Co(cod4); }
void Co5( sCell cod1,sCell cod2,sCell cod3,sCell cod4,sCell cod5 )
{ Co4(cod1,cod2,cod3,cod4); ; Co(cod5); }
void Co6( sCell cod1,sCell cod2,sCell cod3,sCell cod4,sCell cod5,sCell cod6 )
{ Co5(cod1,cod2,cod3,cod4,cod5); Co(cod6); }
void Co7( sCell cod1,sCell cod2,sCell cod3,sCell cod4,sCell cod5,sCell cod6,sCell cod7 )
{ Co6(cod1,cod2,cod3,cod4,cod5,cod6);  Co(cod7);}

void  Noop(void) {}  pp(Noop)

void  DoVar(void){   *--Stack= Tos; Tos =(sCell)ip; ip = (sCell*)*rStack++; } pp(DoVar)
void  DoConst(void){  *--Stack= Tos; Tos = *ip; ip = (sCell*)*rStack++; } pp(DoConst)
void Execute()
    {
//	tty_printf("Execute\n");
   ireg =Tos; Tos=*Stack++;
	if ( (ireg&SIGN_PRIM_MASK) == SignPrim ) {
	((proc) (ireg))();
	return;
    	}
    *--rStack = (sCell) ip;
        ip = (sCell *) ireg;
    } pp(Execute)

void DoDefer(){
//	tty_printf("DoDefer\n");
    ireg = *ip;
    if ( (ireg&SIGN_PRIM_MASK) == SignPrim ) {
        ip = (sCell*)*rStack++; // exit
        ((proc)(ireg))();
            return;
        }
         ip =  (sCell *) ireg;

} pp(DoDefer)

void Lit_(){  *--Stack = Tos; Tos = *ip++; } pp(Lit_)
void Lit( sCell val) {Co(pLit_);  *here++ = val;  }
void Compile(){  *here++ = Tos; Tos = *Stack++;  }  pp(Compile)
void LitCo(){  Co(pLit_); Compile();}  pp(LitCo)
void Allot(){ *(sCell*)&here += Tos; Tos = *Stack++;  }  pp(Allot)

void  Exit() {
 ip = (sCell*)*rStack++;
 } pp(Exit)
void  Here(void){  *--Stack = Tos; Tos = (sCell)here;  } pp(Here)

void Branch(){
 ip = *(sCell**)ip;
 } pp(Branch)

void QBranch(){
    if(Tos) ip++;
    else    ip = *(sCell**)ip;
    Tos =   *Stack++;
} pp(QBranch)

void Str() {
    *--Stack = Tos;
    *--Stack = (Cell)ip+1;
        Tos = *(char unsigned *)ip;
    ip = (sCell*) ((Cell)ip + Tos + 1);
    ip = (sCell*) ( ( (Cell)ip + sizeof(Cell) - 1 ) & (-sizeof(Cell) )  );

}  pp(Str)

void Dup(){   *--Stack= Tos;  } pp(Dup)
void Drop(){  Tos = *Stack++;  } pp(Drop)
void Nip(){   Stack++;   } pp(Nip)
void QDup(){   if(Tos) *--Stack= Tos;   } pp(QDup)
void Over(){   *--Stack= Tos; Tos = Stack[1];    } pp(Over)
void Tuck(){    sCell tt=*Stack; *Stack=Tos; *--Stack=tt;  }  pp(Tuck)
void Pick(){    Tos = Stack[Tos];  }  pp(Pick)
void i2dup(){   *--Stack= Tos; *--Stack= Stack[1];  } pp(i2dup)
void i2over(){   *--Stack= Tos;  *--Stack= Stack[3]; Tos= Stack[3];  } pp(i2over)
void i2drop(){  Stack++; Tos = *Stack++; } pp(i2drop)
void Swap(){  sCell tt=Tos; Tos=Stack[0]; Stack[0]=tt;  }  pp(Swap)
void i2Swap(){
    sCell   tt=Tos; Tos=Stack[1]; Stack[1]=tt;
            tt=Stack[0]; Stack[0]=Stack[2]; Stack[2]=tt;  }  pp(i2Swap)
void Rot(){ Cell tt=Stack[1]; Stack[1]=Stack[0]; Stack[0]=Tos; Tos=tt; } pp(Rot)
void Add(){ Tos += *Stack++;  } pp(Add)
void Sub(){ Tos = -Tos;  Tos += *Stack++; } pp(Sub)
void Negate(){ Tos = -Tos; } pp(Negate)
void Invert(){ Tos = ~Tos; } pp(Invert)
void i1Add(){ Tos++; } pp(i1Add)
void i1Sub(){ Tos--; } pp(i1Sub)
void i2Add(){ Tos +=2; } pp(i2Add)
void i2Sub(){ Tos -=2; } pp(i2Sub)
void Mul(){ Tos *= *Stack++; } pp(Mul)
void Div(){ sCell tt=*Stack++; Tos = tt/Tos; } pp(Div)
void i2Mul(){ Tos *= 2; } pp(i2Mul)
void i2Div(){ Tos /= 2; } pp(i2Div)
void Mod(){ sCell tt=*Stack++; Tos = tt%Tos; } pp(Mod)
void UMul(){ Tos = (Cell) Tos * (Cell) *Stack++; } pp(UMul)
void UDiv(){ Cell tt=*Stack++; Tos = tt/(Cell)Tos; } pp(UDiv)
void And(){ Tos &= *Stack++; } pp(And)
void AndC(){ Tos = ~Tos & *Stack++; } pp(AndC)
void Or(){  Tos |= *Stack++; } pp(Or)
void Xor(){ Tos ^= *Stack++; } pp(Xor)
void ARshift(){  Tos = *Stack++ >> Tos ; } pp(ARshift)
void Rshift(){  Tos = *(Cell*)Stack++ >> Tos ; } pp(Rshift)
void Lshift(){  Tos = *Stack++ << Tos ; } pp(Lshift)

void Blank(void);
void Blanks(int);

void UDot() {
    uint8_t buffer [44];
    uint8_t* p = buffer;

    size_t s = Tos;

    do {
        ++p;
        s = s / numericBase;
    } while(s);

    *p = ' ';
    p[1] = '\0';

    do {
	__uint8_t nn= (Tos % numericBase);
	if(nn<10)  *--p = '0' + nn;
	else  *--p = 'A' + nn - 10 ;
        Tos = Tos / numericBase;
    } while(Tos);
    Blanks(strlen(buffer)); 
	vgaprint(buffer, text_color);
//    tty_puts(buffer);
    Tos = *Stack++; 
}   pp(UDot)

void Dot() {
 if(Tos<0){tty_puts("-"); Negate();} 
 UDot();
}   pp(Dot)

void Load(){  Tos =  *(Cell*)Tos;  } pp(Load)
void Store(){ *(Cell*)Tos = *Stack++;  Tos = *Stack++;} pp(Store)
void CLoad(){ Tos =  (Cell)*(__uint8_t*) Tos;  } pp(CLoad)
void CStore(){ *(__uint8_t*)Tos = (__uint8_t)*Stack++; Tos = *Stack++;  } pp(CStore)
void CAddStore(){ *(__uint8_t*)Tos += (__uint8_t)*Stack++; Tos = *Stack++;  } pp(CAddStore)
void CStoreA(){ *(__uint8_t*)Tos = (__uint8_t)*Stack++; } pp(CStoreA)
void WLoad(){ Tos =  (Cell)*(__uint16_t*) Tos;  } pp(WLoad)
void WStore(){ *(__uint16_t*)Tos = (__uint16_t)*Stack++; Tos = *Stack++;  } pp(WStore)
void LLoad(){ Tos =  (Cell)*(__uint32_t*) Tos;  } pp(LLoad)
void LStore(){ *(__uint16_t*)Tos = (__uint32_t)*Stack++; Tos = *Stack++;  } pp(LStore)

void i2Store(){
 uint128_t val = ((uint128_t)(Cell)Stack[1]<<64) + (uint128_t)(Cell)Stack[0];
   *(uint128_t*)Tos = val;
   Stack += 2 ;  Tos = *Stack++;} pp(i2Store)

void i2Load(){  uint128_t val = *(uint128_t*)Tos; Tos= val; *--Stack=val>>64;} pp(i2Load)

void AddStore(){ *(Cell*)Tos += *Stack++;  Tos = *Stack++;} pp(AddStore)
void Count(){ *--Stack = Tos+1; Tos = (sCell) *(char *)Tos; } pp(Count)
void On(){  *(Cell*)Tos = -1; Tos = *Stack++; } pp(On)
void Off(){ *(Cell*)Tos = 0; Tos = *Stack++;  } pp(Off)
void Incr(){  *(Cell*)Tos += 1; Tos = *Stack++; } pp(Incr)
void ZEqual(){ Tos = -(Tos==0); } pp(ZEqual)
void ZNEqual(){ Tos = -(Tos!=0); } pp(ZNEqual)
void DZEqual(){  Tos = -( (Tos | *Stack++) == 0); } pp(DZEqual)
void ZLess(){ Tos = -(Tos<0); } pp(ZLess)
void Equal(){  Tos = -(*Stack++==Tos); } pp(Equal)
void NEqual(){  Tos = -(*Stack++!=Tos); } pp(NEqual)
void Less(){   Tos = -(*Stack++<Tos);  } pp(Less)
void Great(){  Tos = -(*Stack++>Tos);  } pp(Great)
void ULess(){  Tos = -((Cell)*Stack++ < (Cell)Tos); } pp(ULess)
void UGreat(){ Tos = -((Cell)*Stack++ > (Cell)Tos); } pp(UGreat)

void Max(){ sCell tt = *Stack++; if(tt>Tos) Tos=tt; } pp(Max)
void Min(){ sCell tt = *Stack++; if(tt<Tos) Tos=tt; } pp(Min)
void i0Max(){  if(Tos<0) Tos=0; } pp(i0Max)

void ToR(){   *--rStack = Tos; Tos = *Stack++; }	pp(ToR)
void RLoad(){ *--Stack = Tos; Tos = *rStack; }		pp(RLoad)
void FromR(){ *--Stack = Tos; Tos = *rStack++; }	pp(FromR)
void i2ToR(){  *--rStack = *Stack++; *--rStack = Tos ; Tos = *Stack++; } pp(i2ToR)
void i2RLoad(){ *--Stack = Tos; Tos = *rStack; *--Stack = rStack[1];	  } pp(i2RLoad)
void i2FromR(){ *--Stack = Tos; Tos = *rStack++; *--Stack = *rStack++;	  } pp(i2FromR)
void RDrop(){ *rStack++; }    pp(RDrop)
void RPGet(){ *--Stack = Tos; Tos = (Cell) rStack; } pp(RPGet)
void SPGet(){ *--Stack = Tos; Tos = (Cell) Stack ; } pp(SPGet)
void RPSet(){   rStack = (sCell*)Tos; Tos = *Stack++; } pp(RPSet)
void SPSet(){    Stack = (sCell*)(Tos+sizeof(sCell)); Tos = Stack[-1]; } pp(SPSet)

void ZCount(){ *--Stack= Tos; Tos = strlen((char *)Tos); } pp(ZCount)

void Emit() {
	Blank();
         vgaprint((const char[]){(char)Tos,'\0'}, text_color);
//	tty_puts((const char[]){(char)Tos,'\0'});
 Tos = *Stack++; } pp(Emit)

void Space() {
	Blank();
    uint32_t cur_x;
    uint32_t cur_y;
	get_cursor_pos(&cur_x ,&cur_y);
	set_cursor_pos(cur_x+1,cur_y);

//	tty_puts(" ");
 } pp(Space)

void Spaces() {
	Blanks(Tos);
    uint32_t cur_x;
    uint32_t cur_y;
	get_cursor_pos(&cur_x ,&cur_y);
	set_cursor_pos(cur_x+Tos,cur_y);
	Tos = *Stack++;

//	tty_puts(" ");
 } pp(Spaces)

void Cr() { tty_puts("\n"); } pp(Cr)


void Type() {
	char * str = (char *) *Stack;
    Blanks(Tos); 
    for (size_t i = 0; i < Tos; i++) {
//         kprint((const char[]){str[i],'\0'}, text_color);
	vgaprint((const char[]){str[i],'\0'}, text_color);
//	tty_puts((const char[]){str[i],'\0'});
    } // punch();
	*Stack++;
    Tos =  *Stack++;
} pp(Type)

void ZType() {
    Blanks(strlen((char*)Tos));
 vgaprint((char*)Tos,text_color); Tos = *Stack++;} pp(ZType)

void SetXY() // ( x y -- )
{
	set_cursor_pos( *(uint32_t *)Stack , (uint32_t)Tos);
	Stack++; Tos = *Stack++;
} pp(SetXY)

void GetXY() // ( -- x y )
{	*--Stack = Tos;	*--Stack=0; Tos=0;
	get_cursor_pos( (uint32_t *)Stack , (uint32_t *)&Tos);
} pp(GetXY)


void Ahead(){ Co(pBranch); *--Stack = Tos; Tos = (sCell)here; Co(0);} pp(Ahead)
void If(){ Co(pQBranch); *--Stack = Tos; Tos= (sCell)here; Co(0);} pp(If)
void Then(){  *(sCell**)Tos++ = here; Tos = *Stack++; } pp(Then)
void Else(){  Ahead();    Swap(); Then(); } pp(Else)
void Begin(){ *--Stack = Tos; Tos =  (sCell)here; } pp(Begin)
void Until(){ Co(pQBranch);   *here++ = (sCell)Tos; Tos = *Stack++; } pp(Until)
void Again(){ Co(pBranch);  *here++ = (sCell)Tos; Tos = *Stack++; } pp(Again)
void While(){ If(); Swap(); } pp(While)
void Repeat(){ Again(); Then(); }   pp(Repeat)

void DNegate(){ __int128 val =
 -(__int64_t)( ((uint128_t)(Cell)Tos<<64) + (uint128_t)(Cell)Stack[0] ) ;
	Tos= val>>64;
	Stack[0]=val;
  } pp(DNegate)

void DAbs(){   if(Tos<0) DNegate();  } pp(DAbs)

void DAdd()
{ uint128_t sum= ((uint128_t)(Cell)Tos<<64) + (uint128_t)(Cell)Stack[0] +
	 ((uint128_t)(Cell)Stack[1]<<64) + (uint128_t)(Cell)Stack[2];
	Stack += 2 ;
	Tos= sum>>64;
	Stack[0]=sum;
} pp(DAdd)

void UMMul()
{ uint128_t mul= (uint128_t)(Cell)Tos * (uint128_t)(Cell)Stack[0] ;
	Tos= mul>>64;
	Stack[0]=mul;
} pp(UMMul)

/* Divide 128-bit unsigned number (high half *b, low half *c) by
   64-bit unsigend number in *a. Quotient in *b, remainder in *c.
*/
static void udiv(uint64_t a,uint64_t *b,uint64_t *c)
{
 uint64_t d,qh,ql;
 int i,cy;
 qh=*b;ql=*c;d=a;
 if(qh==0) {
  *b=ql/d;
  *c=ql%d;
 } else {
  for(i=0;i<64;i++) {
   cy=qh&0x8000000000000000;
   qh<<=1;
   if(ql&0x8000000000000000)qh++;
   ql<<=1;
   if(qh>=d||cy) {
    qh-=d;
    ql++;
    cy=0;
   }
   *c=qh;
   *b=ql;
  }
 }
}

void UMMOD()
{	if(Tos<=*Stack) { /*overflow */
	*++Stack=-1;
	 Tos = -1; return;
        }
        udiv(Tos,(uint64_t *)Stack,(uint64_t *)&Stack[1]);
        Tos = *Stack++;
} pp(UMMOD)

void DIVMOD() // n1 n2 -- rem quot
{	sCell tt=*Stack;
        *Stack = tt%Tos ;
	Tos = tt/Tos;
} pp(DIVMOD)

void Align()
{   Cell sz = ( sizeof (Cell) - 1 ) ;
    char * chere = (char *)here;
    while( (Cell) chere & sz ) *chere++ = 0 ;
    here = (sCell *)chere;
}
// CODE FILL ( c-addr u char -- ) \ 94
void Fill()
{    Cell len =  *Stack++;
    __uint8_t *adr = (__uint8_t *) *Stack++;
  while (len-- > 0)  *adr++ = (__uint8_t)Tos;
  Tos =  *Stack++;
}  pp(Fill)

void Cmove()
{
  __uint8_t *c_to = (__uint8_t *) *Stack++;
  __uint8_t *c_from =(__uint8_t *) *Stack++;
  while (Tos-- > 0)
    *c_to++ = *c_from++;
  Tos =  *Stack++;
}  pp(Cmove)

void Cmove_up()
{
  __uint8_t *c_to = (__uint8_t *) *Stack++;
  __uint8_t *c_from =(__uint8_t *) *Stack++;
  while (Tos-- > 0)
    c_to[Tos] = c_from[Tos];
  Tos =  *Stack++;
}  pp(Cmove_up)

void StrComp(const char * s, sCell len)
{   char * chere = (char *)here;
    len &= 0xff ;
    *chere++ = (char)len;                /* store count byte */
    while (--len >= 0)          /* store string */
        *chere++ = *s++;

    here = (sCell *)chere;
    Align();
}

void StrCmp(){  StrComp((char *) *Stack++, Tos); Tos = *Stack++; } pp(StrCmp)

void ZStrCmp()
{
   const char * s = (char *) *Stack++;
   char * chere = (char *)here;
    while (--Tos >= 0)          /* store string */
        *chere++ = *s++;
    *chere++ = '\0';
    here = (sCell *)chere;
    Align();
     Tos = *Stack++;
} pp(ZStrCmp)

void ZStr() {
    *--Stack = Tos;
    Tos = (sCell)ip;
    ip = (sCell*) ( ( (Cell)ip + strlen((uint8_t*)ip) + sizeof(Cell) - 1 )
	& (-sizeof(Cell) ) );
}  pp(ZStr)

void Tp(const char * s) {
    Co(pStr);
    StrComp(s, strlen(s));
    Co(pType);
}

void SpSet(){    Stack = (sCell*)*Stack; } pp(SpSet)

sCell  ForthWordlist[] = {0,0,0};

#define CONTEXT_SIZE 10
const Cell ContextSize = CONTEXT_SIZE;
sCell * Context[CONTEXT_SIZE] = {ForthWordlist};
sCell * Current[] = {ForthWordlist};

sCell * Last;
sCell * LastCFA;

void  WordBuild (const char * name, sCell cfa )
{
    LastCFA=here;
    Co(cfa);
    Co(0); // flg
    Co(** (sCell **) Current);
    Last=here;
    StrComp(name, strlen(name));
}

void Smudge(){ **(sCell***) Current=Last; } pp(Smudge)

void Immediate(){ Last[-2] |= 1; } pp(Immediate)

void FthItem (const char * name, sCell cfa ){
    WordBuild (name, cfa );
    Smudge();
}

sCell Header(const char * name) {
    FthItem (name,0);
    *(sCell **)LastCFA = here;
    return  *(sCell *)LastCFA;
}

sCell Variable (const char * name ) {
    FthItem(name,0);
    *(sCell **) LastCFA = here;
    *here++ =  pDoVar;
    *here++ = 0;
    return  *(sCell *)LastCFA;
}

sCell VVariable (const char * name, sCell val ) {
    FthItem(name,0);
    *(sCell **) LastCFA = here;
    *here++ = pDoVar;
    *here++ = val;
    return  *(sCell *)LastCFA;
}

sCell Constant (const char * name, sCell val ) {
    FthItem(name,0);
    *(sCell **) LastCFA = here;
    *here++ = pDoConst;
    *here++ = val;
    return  *(sCell *)LastCFA;
}

char atib[256]={"atib atib qwerty"};
sCell *ptib;
sCell ntib;
void Source(){
 *--Stack = Tos;
 *--Stack = *ptib;
    Tos =  ntib;
  } pp(Source)

void SourceSet(){
  ntib = Tos;
 *ptib = *Stack++;
 Tos = *Stack++;
  } pp(SourceSet)

// ALLOCATE ( u -- a-addr ior ) 
void Allocate()
{
	*--Stack= Tos;
	
	*Stack= (sCell) kmalloc(Tos);
	LOG_INFO("kmalloc=%p %p\n",*Stack,Tos);
	Tos=0;
  	if(*Stack==0) Tos=-59;
	LOG_INFO("To=%p\n",Tos);

} pp(Allocate)

void Free()
{
	LOG_INFO("kfree=%p\n",Tos);
	kfree((void*)Tos);
	Tos=0;

} pp(Free)

sCell *v2in;

void Accept() // ( c-addr +n -- +n' )
{
    char * command = (char *)*(Stack++);
    int cmd_idx = 0;

    for (;;) {
        char c = keyboard_getchar(); 
        if (c != 0) {
            if (c == '\n') {
                kprint("\n", 7);
                command[cmd_idx] = '\0';
//   serial_print("@"); serial_hex64((Cell)command); serial_print(": "); serial_hex64(cmd_idx);
//   serial_print(command);
//  serial_print("\n");

		 Tos = cmd_idx;
		return;		
            } else if (c == '\b') {
                if (cmd_idx > 0) {
                    cmd_idx--;
                    command[cmd_idx] = '\0';
                    vga_backspace();
                }
            } else if (c >= 32 && c <= 126) {
                if (cmd_idx < Tos - 1) {
                    command[cmd_idx++] = c;
                    char s[2] = {c, 0};
                    kprint(s, 15);
                }
            }
	}
    }
} pp(Accept)

void ParseName() {
    Cell addr,Waddr,Eaddr;
    addr=  *ptib + *v2in;
    Eaddr= *ptib + ntib;

    *--Stack = Tos;
    while (  addr<Eaddr ) { if( *(__uint8_t*)addr > ' ') break;
        addr++; }
    *--Stack=Waddr=addr;
    *v2in = addr - *ptib;
    while ( addr<=Eaddr ) { (*v2in)++; if( *(__uint8_t*)addr <= ' ') break;
//	serial_print((const char[]){*(char*)addr,'\0'});
     addr++; }
    Tos=addr-Waddr;
//   serial_print("}"); serial_hex64(addr); serial_print(": "); serial_hex64(Tos);  serial_print("\n");
} pp(ParseName)

void Parse() {
    Cell addr,Waddr,Eaddr;
	if(((__uint8_t*)*ptib)[ntib] == '\r' ) ntib--;
    addr=  *ptib  + *v2in;
    Eaddr= *ptib  + ntib;

    char cc = (char)Tos;
    *--Stack=Waddr=addr;
    while ( addr<=Eaddr ) {  (*v2in)++;  if(*(__uint8_t*)addr == cc ) break;
        addr++;}
    Tos=addr-Waddr;
} pp(Parse)

char c_islower (char c)
{
    if  ( c >= 'a' && c <= 'z' ) return 1;
    return 0;
}

char c_toupper(char c)
{
  return c_islower (c) ? c - 'a' + 'A' : c;
}


Cell memcasecmp (const char *vs1, const char *vs2, Cell n)
{
    unsigned int i;
    char const *s1 = (char const *) vs1;
    char const *s2 = (char const *) vs2;
    for (i = 0; i < n; i++)
    {
        char u1 = *s1++;
        char u2 = *s2++;
        if (c_toupper (u1) != c_toupper (u2))
            return c_toupper (u1) - c_toupper (u2);
    }
    return 0;
}

Cell CCompare( void * caddr1  ,  Cell len1 ,  void * caddr2  ,  Cell len2) {
    if (len1 < len2) return -1;
    if (len1 > len2) return  1;

    auto int cmpResult = memcasecmp(caddr1, caddr2, len1);

    if (cmpResult < 0) return -1;
    if (cmpResult > 0) return  1;
    return   0;
}

void UCompare(){ 
	char * caddr1 = (char *) *Stack++;
	sCell  len1 =  *Stack++;
	char * caddr2 = (char *) *Stack++;

    if (len1 != Tos) {  Tos -= len1; return; }

    Tos = memcasecmp(caddr1, caddr2, Tos);  } pp(UCompare)

char *SEARCH(char **wid,  char * word , Cell len)
{ char * addr= (char *) *wid;
    for(;;)
    {   if(!addr)
          { // tty_puts("\n");
  return  NULL; }
        char * caddr = addr ;

//    for (size_t i = 1; i <= (size_t) caddr[0]; i++) {
//	tty_puts((const char[]){caddr[i],'\0'});

//    } // punch();

//	tty_puts(" ");

        if( !CCompare(word, len, caddr+1, *caddr ))
          { // tty_puts("\n");
  return  addr; }
        addr = ((char **)addr)[-1];
    }
}

void FromName(){  Tos=((sCell *)Tos)[-3]; } pp(FromName)

void SearchWordList() // ( c-addr u wid --- 0 | xt 1 xt -1 )
{
    char ** addr=  (char **) Tos;
    Cell  len=Stack[0];
    char * word= (char * ) Stack[1];

    if(!addr) { Stack+=2; Tos=0; return; }
    Cell * nfa= (Cell*) SEARCH(addr,word,len);
    if(!nfa) {
        Stack+=2; Tos=0;
        return;
    }
    Stack++;
    Stack[0]=nfa[-3];
    Tos = nfa[-2]&1 ? 1 : -1;

}  pp(SearchWordList)


void SFind()
{	sCell * voc=  (sCell *) Context;
    *--Stack = Tos;
    while( *voc )
    {	*--Stack = Stack[1];
        *--Stack = Stack[1]; Tos=*voc;
        SearchWordList();
        if(Tos)
        {   Stack[2]=Stack[0];  Stack+=2; // 2nip
            return;
        }   voc++;
    }

} pp(SFind)

Cell State;

void StateQ(){ *--Stack= Tos; Tos = State; } pp(StateQ)

void IMode(){ State = 0;}  pp(IMode)
void CMode(){ State = -1;}  pp(CMode)

sCell * YDP;
sCell * YDP0;

sCell YDPFL[] = { pDoConst, 0 }; pp(YDPFL)

void QYDpDp()
{
  if(YDPFL[1] == 0) return;
   sCell * tmp = YDP ;
    YDP = here ;
    here = tmp ;
}

void SBuild()
{    char * name = (char * ) *Stack++ ;
	QYDpDp();
    LastCFA=here;
    Co(0);
    Co(0); // flg
    Co(** (sCell  **) Current);
    Last=here;
    StrComp(name, Tos);
    Tos = *Stack++;
	QYDpDp();
    *(sCell **)LastCFA = here;
}

void Build()
{ //   *--Stack = Tos; Tos=(sCell)pNoop;
    ParseName();
    SBuild();
} pp(Build)

void SHeader()
{
	SBuild();
	Smudge();  
} pp(SHeader)


void SNumber0() // ( str len -- m flg )
{
    char* rez;
    char  NumStr[44];
    sCell signedFlg = 1;
    Cell len = Tos;
    char * caddr = (char*) Stack[0];
    if(caddr[0]=='-') { len--; caddr++; signedFlg = -1; }
    NumStr[len]=0;
    while(len){ --len; NumStr[len] = caddr[len]; }
    *Stack = (sCell) strtoul( NumStr,  &rez, numericBase) * signedFlg;
    Tos =  strlen(rez);
}  pp(SNumber0)

void Colon(){
  Build();
  CMode(); } pp( Colon)
void Semicolon(){ Co(pExit); Smudge(); IMode(); } pp(Semicolon)

void to_catch(){
    *--rStack = (sCell)Handler;
    *--rStack = (sCell)Stack;
    Handler = rStack;
    Execute();
} pp(to_catch)

void from_catch(){
    rStack++;
    Handler = (sCell*)*rStack++;
    *--Stack = Tos;  Tos = 0;
    ip = (sCell*)*rStack++; // exit
} pp(from_catch)

void FThrowDo()
{   *--Stack = Tos;
    if (Handler == NULL); //  TODO("Handler=0")
    rStack =   Handler ;
    Stack = (sCell*)*rStack++;
    Handler = (sCell*)*rStack++;
    ip = (sCell * ) *rStack++;
}

void FThrow(){
    if (Tos == 0){  Tos = *Stack++; return;  }
    FThrowDo();
} pp(FThrow)

sCell Lastin =0;
sCell SaveErrQ = -1;
sCell ErrIn;

void SaveErr0()
{ if(SaveErrQ & Tos )
    {  SaveErrQ = 0;
       ErrIn = *v2in ;
    }

} pp(SaveErr0)


void PrintErr0()
{  numericBase = 10;
    tty_puts("Err="); Dot();
     SaveErrQ=-1;
} pp(PrintErr0)

// R/O ( -- fam )
void readOnly() { *--Stack = Tos; Tos = VFS_READ; }  pp(readOnly)

// R/W ( -- fam )
void readWrite() { *--Stack = Tos; Tos = VFS_WRITE | VFS_READ; } pp(readWrite)

// W/O ( -- fam )
void writeOnly() { *--Stack = Tos; Tos = VFS_WRITE ; } pp(writeOnly)

typedef struct FILEID {
	vfs_mount_t* mnt;
	vfs_file_handle_t h;
} FILEID;

// OPEN-FILE ( c-addr u fam -- fileid ior )
void openFile() {
    Cell flen = *Stack++;
    const char* path = (char*)*Stack;

    if (*path == '\0') {
	Tos = -69;
        return;
    }

    char full_path[MAX_PATH_LENGTH];
    char* ppath = full_path;
    if (path[0] != '/') {
        strcpy(full_path, current_working_directory);
        if (full_path[strlen(full_path)-1] != '/')
            strcat(full_path, "/");
        ppath = &full_path[strlen(full_path)];
    }

    while(flen--) *ppath++ = *path++;
   *ppath++='\0';
        kprint(full_path, 17);

    const char* rel = NULL;
    vfs_mount_t* mnt = vfs_find_mount(full_path, &rel);

    if (!(mnt && mnt->fs && mnt->fs->ops)) {
	Tos = -691;
        return;
    }
    const vfs_fs_ops_t* ops = mnt->fs->ops;
    vfs_stat_t st;
    int rc = ops->stat ? ops->stat(mnt, rel, &st) : -1;

 serial_print("o>"); serial_hex64((Cell)rc); serial_print(": "); serial_hex64((Cell)st.st_mode);  serial_print("\n");

    if (!(rc == 0 && (st.st_mode & 0xF000) != 0x4000)) {
	Tos = -692;
        return;
    }

    if (!ops->open) {
	Tos = -693;
        return;
    }

    FILEID* fid = kmalloc(sizeof(FILEID));
    memset(&fid->h, 0, sizeof(vfs_file_handle_t));
    fid->h.position = 0;
    fid->h.flags = Tos;
    Tos = ops->open(mnt, rel, Tos, &fid->h);

    if (Tos) {
        kfree(fid);
	Tos = -694;
        return;
    }
    fid->mnt = mnt;
    *Stack = (sCell)fid;
    Tos=0;    
/*
                char buf[256];
                vfs_ssize_t n;
                while ((n = ops->read(fid->mnt, &fid->h, buf, sizeof(buf))) > 0) {
                    for (vfs_ssize_t i = 0; i < n; i++) {
                        char c[2] = {buf[i], '\0'};
                        kprint(c, 15);
                    }
                }
*/

} pp(openFile)

// CLOSE-FILE ( fileid -- ior )
void closeFile() {
    FILEID* fid = (FILEID*) Tos;
    const vfs_fs_ops_t* ops = fid->mnt->fs->ops;

    if (ops->close) ops->close(fid->mnt, &fid->h);
    kfree(fid);
    Tos = 0;
} pp(closeFile)


// READ-FILE ( c-addr u1 fileid -- ior )
void readFile() {
    Cell len = *Stack++;
    char * buf = (char*) *Stack;
//                char buf[256];
    FILEID* fid = (FILEID*) Tos;
    const vfs_fs_ops_t* ops = fid->mnt->fs->ops;
    *Stack = ops->read(fid->mnt, &fid->h, buf, len);
    Tos = 0;
    if(*Stack<0) Tos = -70; 

} pp(readFile)

// READ-LINE ( c-addr u1 fileid -- u2 flag ior )
void readLine() {
	
    Cell len = *Stack;
    char * buf = (char*)Stack[1];
    FILEID* fid = (FILEID*) Tos;
    const vfs_fs_ops_t* ops = fid->mnt->fs->ops;

    Stack[1] = ops->read(fid->mnt, &fid->h, buf, len);
    if(Stack[1]<0){ Stack[1]=0;	*Stack=0; Tos=-70;  return; }

	Tos = 0;

	if(Stack[1]==0){ *Stack=0; return; }

	len = 0;
	while(Stack[1] > len)
	{ if(buf[len]=='\n'){ break;}
	  len++;
	}

	if(buf[len]!='\n'){ *Stack=-1; return; }

	*Stack = ops->seek(fid->mnt, &fid->h, len-Stack[1]+1 , VFS_SEEK_CUR);

	if(!*Stack) { Stack[1]=0;  return; }

	if(buf[len]=='\r') len--;

//	buf[len]='\0';
//	LOG_INFO(buf);
//	LOG_INFO("%x %x\n",(int)buf[len-2],(int)buf[len-1]);

	Stack[1]=len;
	*Stack= -1;

} pp(readLine)


// WRITE-FILE ( c-addr u1 fileid -- ior )
void writeFile() {
    Cell len = *Stack++;
    char * buf = (char*) *Stack;
    FILEID* fid = (FILEID*) Tos;
    const vfs_fs_ops_t* ops = fid->mnt->fs->ops;

    *Stack = ops->write(fid->mnt, &fid->h, buf, len);
    Tos = 0;
    if(*Stack<0) Tos = -70; 

} pp(writeFile)

void  Bye(void) {forth_run=0;}  pp(Bye)

void  Hello(void) { 	tty_puts("__Hello___\n"); }  pp(Hello)

//void  LogInfo(void) { LOG_INFO((void*)Tos); Tos = *Stack++; }  pp(LogInfo)
void  LogInfo(void) { LOG_INFO("%s\n",(void*)Tos);  Tos = *Stack++; }  pp(LogInfo)
void  LogInfoN(void) { LOG_INFO("%s%p\n",(void*)Tos,(void*)*Stack++);  Tos = *Stack++; }  pp(LogInfoN)

const char *initScript =
        " HI : 2NIP 2SWAP 2DROP ;\n"
        " : COMPILE, , ;\n"
        " : HEX 16 BASE ! ;\n"
        ": DECIMAL 10 BASE ! ;\n"
        ": HEADER BUILD SMUDGE ;\n"
        ": CONSTANT HEADER DOCONST , , ;\n"
        ": CREATE HEADER DOVAR , ;\n"
        ": VARIABLE CREATE 0 , ;\n"
        ": [COMPILE] ' , ; IMMEDIATE\n"
        ": CELL+ CELL + ;\n"
        ": CELL- CELL - ;\n"
        ": CELLS CELL * ;\n"
        ": >BODY CELL+ ;\n"
        ": COMPILE R> DUP @ , CELL+ >R ;\n"
        ": CHAR  PARSE-NAME DROP C@ ;\n"
        ": [CHAR] CHAR LIT,  ; IMMEDIATE\n"
        ": [']  ' LIT, ; IMMEDIATE\n"
        ": .( [CHAR] ) PARSE TYPE ; IMMEDIATE\n"
        ": ( [CHAR] ) PARSE 2DROP ; IMMEDIATE\n"
        ": SLIT, ( string -- ) COMPILE <$> $, ;\n"
        ": ZLIT, ( string -- ) COMPILE <Z> Z, ;\n"
        ": \\ 10 PARSE 2DROP  ; IMMEDIATE\n"
        ": .\\ 10 PARSE TYPE cr ; IMMEDIATE\n"
        ": .\" [CHAR] \" PARSE SLIT, COMPILE TYPE   ; IMMEDIATE\n"
        ": S\" [CHAR] \" PARSE ?STATE IF SLIT, THEN ; IMMEDIATE\n"
        ": Z\" [CHAR] \" PARSE ?STATE IF ZLIT, EXIT THEN OVER + 0 SWAP C! ; IMMEDIATE\n"

        ": ABORT -1 THROW ;\n"
        ": H. BASE @ SWAP HEX U. BASE ! ;\n"
        ": POSTPONE\n" // 94
        "  PARSE-NAME SFIND DUP\n"
        "  0= IF -321 THROW THEN \n"
        "  1 = IF COMPILE,\n"
        "      ELSE LIT, ['] COMPILE, COMPILE, THEN\n"
        "; IMMEDIATE\n"
        ": TO '\n"
        "   ?STATE 0= IF >BODY ! EXIT THEN\n"
        "    >BODY LIT, POSTPONE ! ; IMMEDIATE\n"
	": ERASE 0 FILL ;\n"
	": $!\n" //	( addr len dest -- )
	"SWAP 255 AND SWAP	2DUP C! 1+ SWAP CMOVE ;\n"
        ": DEFER@  ( xt1 -- xt2 )  >BODY @ ;\n"
        ": VALUE CONSTANT ;\n"
        ": (DO)   ( n1 n2 ---)\n"
        // Runtime part of DO.
        " R> ROT ROT SWAP >R >R >R ;\n"
        ": (?DO)  ( n1 n2 ---)\n"
        // Runtime part of ?DO
        "  OVER OVER - IF R> ROT ROT SWAP >R >R CELL+ >R \n"
        "                 ELSE DROP DROP R> @ >R\n" // Jump to leave address if equal
        "                 THEN ;\n"
        ": I ( --- n )\n"
        // Return the counter (index) of the innermost DO LOOP
        "  POSTPONE R@ ; IMMEDIATE\n"
        ": Z\\ 10 PARSE H. H. ; IMMEDIATE\n"

        ": J  ( --- n)\n"
        // Return the counter (index) of the next loop outer to the innermost DO LOOP
        " RP@ 3 CELLS + @ ;\n"
        "VARIABLE 'LEAVE ( --- a-addr)\n" // This variable is  used  for  LEAVE address resolution.

        ": (LEAVE)   ( --- )\n"
        // Runtime part of LEAVE
        " R> @ R> DROP R> DROP >R ;\n" // Remove loop parameters and replace top of ret\n"
        // stack by leave address.\n"

        ": UNLOOP ( --- )\n"
        // Remove one set of loop parameters from the return stack.
        "   R> R> DROP R> DROP >R ;\n"

        ": (LOOP) ( ---)\n"
        // Runtime part of LOOP
        "  R> R> 1+ DUP R@ = \n"   // Add 1 to count and compare to limit.
        "  IF \n"
        "   R> DROP DROP CELL+ >R\n" // Discard parameters and skip leave address.
        "  ELSE \n"
        "   >R @ >R\n" // Repush counter and jump to loop start address.
        "  THEN ;\n"

        ": (+LOOP) ( n ---)\n"
        // Runtime part of +LOOP
        // Very similar to (LOOP), but the compare condition is different.
        //  exit if ( oldcount - lim < 0) xor ( newcount - lim < 0).
        "     R> SWAP R> DUP R@ - ROT ROT + DUP R@ - ROT XOR 0 < \n"
        "     IF R> DROP DROP CELL+ >R\n"
        "     ELSE >R @ >R THEN ;\n"

        ": DO ( --- x)\n"
        // Start a DO LOOP.
        // Runtime: ( n1 n2 --- ) start a loop with initial count n2 and
        // limit n1.
        "  POSTPONE (DO) 'LEAVE @  HERE 0 'LEAVE ! \n"
        "   ; IMMEDIATE\n"

        ": ?DO  ( --- x )\n"
        // Start a ?DO LOOP.\n"
        // Runtime: ( n1 n2 --- ) start a loop with initial count n2 and
        // limit n1. Exit immediately if n1 = n2.
        "  POSTPONE (?DO)  'LEAVE @ HERE 'LEAVE ! 0 , HERE ; IMMEDIATE\n"

        ": LEAVE ( --- )\n"
        // Runtime: leave the matching DO LOOP immediately.
        // All places where a leave address for the loop is needed are in a linked\n"
        // list, starting with 'LEAVE variable, the other links in the cells where
        // the leave addresses will come.
        "  POSTPONE (LEAVE) HERE 'LEAVE @ , 'LEAVE ! ; IMMEDIATE\n"
        ": RESOLVE-LEAVE\n"
        // Resolve the references to the leave addresses of the loop.
        "         'LEAVE @\n"
        "         BEGIN DUP WHILE DUP @ HERE ROT ! REPEAT DROP ;\n"

        ": LOOP  ( x --- )\n"
        // End a DO LOOP.
        // Runtime: Add 1 to the count and if it is equal to the limit leave the loop.
        " POSTPONE (LOOP) ,  RESOLVE-LEAVE  'LEAVE ! ; IMMEDIATE\n"

        ": +LOOP  ( x --- )\n"
        // End a DO +LOOP
        // Runtime: ( n ---) Add n to the count and exit if this crosses the
        // boundary between limit-1 and limit.
        " POSTPONE (+LOOP) , RESOLVE-LEAVE 'LEAVE ! ; IMMEDIATE\n"

        ": (;CODE) ( --- )\n"
        // Runtime for DOES>, exit calling definition and make last defined word
        // execute the calling definition after (;CODE)
        "  R> LAST @  NAME>  ! ;\n"

        ": DOES>  ( --- )\n"
        // Word that contains DOES> will change the behavior of the last created
        // word such that it pushes its parameter field address onto the stack
        // and then executes whatever comes after DOES>
        " POSTPONE (;CODE) \n"
        " POSTPONE R>\n" // Compile the R> primitive, which is the first
        // instruction that the defined word performs.
        "; IMMEDIATE\n"

    ": SET-CURRENT ( wid -- )\n" // 94 SEARCH
    "        CURRENT ! ;\n"

    ": GET-CURRENT ( -- wid )\n" // 94 SEARCH
    "        CURRENT @ ;\n"

    ": GET-ORDER ( -- widn ... wid1 n )\n"  // 94 SEARCH
        " SP@ >R 0 >R\n"
        " CONTEXT\n"
        " BEGIN DUP @ ?DUP\n"
        " WHILE >R CELL+\n"
        " REPEAT  DROP\n"
        " BEGIN R> DUP 0=\n"
        " UNTIL DROP\n"
        "R> SP@ - CELL / 1- ; \n"

	" HERE S\" FORTH\" $, FORTH-WORDLIST CELL+ !\n"

        ": VOC-NAME. ( wid -- )\n"
        "DUP CELL+ @ DUP IF COUNT TYPE BL EMIT DROP ELSE DROP .\" <NONAME>:\" U. THEN ;\n"

        ": ORDER ( -- )\n" // 94 SEARCH EXT
        "GET-ORDER .\" Context: \" \n"
        "0 ?DO ( DUP .) VOC-NAME. SPACE LOOP CR\n"
        ".\" Current: \" GET-CURRENT VOC-NAME. CR ;\n"

        ": SET-ORDER ( wid1 ... widn n -- )\n"
        "DUP -1 = IF\n"
        "DROP  FORTH-WORDLIST 1\n"
        "THEN\n"
        "DUP  CONTEXT-SIZE  U> IF -49 THROW THEN\n"
        "DUP CELLS context + 0!\n"
        "0 ?DO I CELLS context + ! LOOP ;\n"
        "CREATE VOC-LIST FORTH-WORDLIST CELL+ CELL+ ,\n"

        ": FORTH FORTH-WORDLIST CONTEXT ! ;\n"
        ": DEFINITIONS  CONTEXT @ CURRENT ! ;\n"

        ": WORDLIST ( -- wid )\n" // 94 SEARCH
        " HERE 0 , 0 ,\n"
        " HERE VOC-LIST  @ , .\" W=\" DUP H.  VOC-LIST ! ;\n"

	": ONLY ( -- ) -1 SET-ORDER ;\n"
	": ALSO ( -- )   GET-ORDER OVER SWAP 1+ SET-ORDER ;\n"
	": PREVIOUS ( -- ) GET-ORDER NIP 1- SET-ORDER ;\n"


	": LATEST ( -> NFA ) CURRENT @ @ ;\n"

	": VOCABULARY ( <spaces>name -- )\n"
	"WORDLIST CREATE DUP ,\n"
	"LATEST SWAP CELL+ !\n"
	"DOES>  @ CONTEXT ! ;\n"
	" VARIABLE CURSTR\n"

	": ->DEFER ( cfa <name> -- )  HEADER DODEFER , , ;\n"
	": DEFER ( <name> -- ) ['] ABORT ->DEFER ;\n"

        ": VECT DEFER ;\n"

	": FQUIT  BEGIN REFILL WHILE\n"
//	"  CR SOURCE TYPE\n"
	"  INTERPRET  REPEAT ;\n"

	": LALIGNED  3 + 3 ANDC ;\n"

	" 255 CONSTANT TC/L\n"
  "444 CONSTANT  CFNAME_SIZE\n"
  "CREATE CURFILENAME  CFNAME_SIZE 255 + 1+ ALLOT\n"
  "CURFILENAME  CFNAME_SIZE 255 + 1+  ERASE\n"

 ": INCLUDE-FILE\n" // ( fid --- )
// Read lines from the file identified by fid and interpret them.
// INCLUDE and EVALUATE nest in arbitrary order.
	"SOURCE-ID >R >IN @ >R LASTIN @ >R CURSTR @ >R CURSTR 0!\n"
	"SOURCE 2>R\n"
	" TC/L ALLOCATE THROW  dup z\" tall=\" LOGINFON TC/L SOURCE!\n"
	"TO SOURCE-ID\n"
	"['] FQUIT CATCH SAVEERR\n"
	"DUP IF cr .\" in <\" CURFILENAME COUNT TYPE .\" >\" CURSTR @ . THEN\n"
	"TIB FREE DROP\n"
	"2R> SOURCE!\n"

	"R> CURSTR ! R> LASTIN ! R> >IN ! R> TO SOURCE-ID\n"
	"THROW ;\n"

	": FREFILL0\n" // (  -- flag )
	"  TIB TC/L SOURCE-ID READ-LINE THROW\n"
	"  SWAP  #TIB !  0 >IN ! CURSTR 1+!\n"
	"  0 SOURCE + C! ;\n"
	"' FREFILL0 TO FREFILL\n"

  ": CFNAME-SET\n" // ( adr len -- )
  "DUP 1+ >R  CURFILENAME CURFILENAME R@ + CFNAME_SIZE R> - CMOVE>\n"
  "CURFILENAME $! ;\n"

  ": CFNAME-FREE\n" //  ( -- )
  "CURFILENAME COUNT + CURFILENAME\n"
  "CFNAME_SIZE CURFILENAME C@ - 255 +  CMOVE ;\n"

 ": INCLUDED\n" // ( c-addr u ---- )
 "2DUP CFNAME-SET\n"
 "R/O OPEN-FILE THROW\n"
 "DUP >R ['] INCLUDE-FILE CATCH CFNAME-FREE\n"
 "R> CLOSE-FILE DROP THROW ;\n"

 ": EVALUATE\n" // ( i*x c-addr u -- j*x ) \ 94
 "SOURCE-ID >R SOURCE 2>R >IN @ >R\n"
 "-1 TO SOURCE-ID\n"
 "SOURCE! >IN 0!\n"
 "['] INTERPRET CATCH\n"
 "R> >IN ! 2R> SOURCE! R> TO SOURCE-ID\n"
 "THROW ;\n"

 ": FLOAD PARSE-NAME INCLUDED ;\n"

 ": [DEFINED]\n" //  ( -- f ) \ "name"
 "PARSE-NAME  SFIND  IF DROP -1 ELSE 2DROP 0 THEN ; IMMEDIATE\n"

 ": [UNDEFINED]\n" //  ( -- f ) \ "name"
 "POSTPONE [DEFINED] 0= ; IMMEDIATE\n"

 ": \\+	POSTPONE [UNDEFINED]	IF POSTPONE \\ THEN ; IMMEDIATE\n"
 ": \\-	POSTPONE [DEFINED]	IF POSTPONE \\ THEN ; IMMEDIATE\n"

 ": BREAK  POSTPONE EXIT POSTPONE THEN ; IMMEDIATE\n"

 ": PRIM? 0< ['] DUP 0< = ;\n"

 ": ?CONST\n" // ( cfa -- cfa flag )
 "DUP PRIM? IF 0 BREAK\n"
 "DUP @ DOCONST = ;\n"

 ": ?VARIABLE\n" // ( cfa -- cfa flag )
 "DUP PRIM? IF 0 BREAK\n"
 "DUP @ DOVAR = ;\n"

  "S\" usr/forth/autoexec.4th\" INCLUDED"

;

/*
: ZLITERAL ( a u -- )
  STATE @ IF
             ['] _ZLITERAL-CODE COMPILE,
             DUP ,
             HERE SWAP DUP ALLOT MOVE 0 C,
          ELSE
             OVER + 0 SWAP C!
          THEN
; IMMEDIATE

: ZLITERAL ( addr u -- \ a)
  STATE @ IF
    POSTPONE ALITERAL
    DUP C,
    HERE SWAP DUP ALLOT MOVE 0 C,
  ELSE
    DUP 1+ ALLOCATE THROW DUP >R CZMOVE R>
  THEN
; IMMEDIATE


: Z" [CHAR] " PARSE [COMPILE] ZLITERAL ; IMMEDIATE


*/

uint32_t CursorHSize = 5;

void Cursor()
{ uint32_t* fb_addr = get_framebuffer();
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t fb_pitch;
    uint32_t h_char;
    uint32_t w_char;
    uint32_t cur_x;
    uint32_t cur_y;

  get_fb_dimensions(&fb_width, &fb_height, &fb_pitch);

    w_char = fb_width / get_screen_width_chars();
    h_char = fb_height / get_screen_height_chars();

//    LOG_INFO("fb_addr=%p %x %x \n",fb_addr,h_char,w_char);

	get_cursor_pos(&cur_x ,&cur_y);

//       h_char *= cur_y+1;
//    for (uint32_t y = h_char-CursorHSize; h_char; y++) {

    uint32_t zcur_y = (cur_y+1)*h_char ;
    uint32_t zcur_x = cur_x*w_char ;

    for (uint32_t y = zcur_y-CursorHSize  ; y < zcur_y; y++) {
        for (uint32_t x = zcur_x; x < zcur_x + w_char; x++) {
//	    LOG_INFO("fb_r=%x\n",y * fb_width + x);
		   fb_addr[y * fb_width + x] ^= -1;
        }
    }


} pp(Cursor)

void  OpenDir() // ( c-addr lem -- dir-id flag )
{   Cell dlen = Tos;
    Cell plen = 0;
    char * caddr = (char*) *Stack;

	if(caddr[0]!='/') plen = strlen(current_working_directory);
	
	char * path = kmalloc(plen+dlen+1);

	path[plen+dlen]=0;
    
    while(dlen){ --dlen; path[plen+dlen] = caddr[dlen]; }

    while(plen){ --plen; path[plen] = current_working_directory[plen]; }

    uint16_t* l_entries = kmalloc(32 * sizeof(vfs_dirent_t)+sizeof(uint16_t));

    if (!entries) { Tos = -1; return; }

    l_entries[0] = vfs_readdir(path,(vfs_dirent_t*)&l_entries[1], 32);
    kfree(path);

	Tos = 0;
	*Stack = (Cell)l_entries;

} pp(OpenDir)

const char* paretnpath = "..";
void  DirI2Name() // ( dir-id n -- z-addr )
{    uint16_t* c_entries =   (uint16_t*)*Stack++ ;
	if(Tos==-1) {Tos =  (sCell)paretnpath; return; }

    vfs_dirent_t* l_entries = (vfs_dirent_t*)&c_entries[1] ;
     Tos = (Cell)l_entries[Tos].d_name;
} pp(DirI2Name)

void  DirI2Type() // ( dir-id n -- n )
{    uint16_t* c_entries =   (uint16_t*)*Stack++ ;
	if(Tos==-1) {Tos = VFS_TYPE_DIR; return; }
    vfs_dirent_t* l_entries = (vfs_dirent_t*)&c_entries[1] ;
     Tos = (Cell)l_entries[Tos].d_type;
} pp(DirI2Type)

void  CloseDir() // ( dir-id -- flg )
{	kfree((void*)Tos);
	Tos = 0;
} pp(CloseDir)

void Blank()
{ uint32_t* fb_addr = get_framebuffer();
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t fb_pitch;
    uint32_t h_char;
    uint32_t w_char;
    uint32_t cur_x;
    uint32_t cur_y;
    uint32_t fb_color = palette_get_color(text_color>>4);

  get_fb_dimensions(&fb_width, &fb_height, &fb_pitch);

    w_char = fb_width / get_screen_width_chars();
    h_char = fb_height / get_screen_height_chars();


	get_cursor_pos(&cur_x ,&cur_y);


    uint32_t zcur_y = (cur_y+1)*h_char ;
    uint32_t zcur_x = cur_x*w_char ;

    for (uint32_t y = zcur_y-h_char ; y < zcur_y; y++) {
        for (uint32_t x = zcur_x; x < zcur_x + w_char; x++) {
		   fb_addr[y * fb_width + x] = fb_color;
        }
    }


} pp(Blank)

void Blanks(int nn)
{ uint32_t* fb_addr = get_framebuffer();
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t fb_pitch;
    uint32_t h_char;
    uint32_t w_char;
    uint32_t cur_x;
    uint32_t cur_y;

    uint32_t fb_color = palette_get_color(text_color>>4);

  get_fb_dimensions(&fb_width, &fb_height, &fb_pitch);

    w_char = fb_width / get_screen_width_chars();
    h_char = fb_height / get_screen_height_chars();


	get_cursor_pos(&cur_x ,&cur_y);


    uint32_t zcur_y = (cur_y+1)*h_char ;
    uint32_t zcur_x = cur_x*w_char ;

    for (uint32_t y = zcur_y-h_char ; y < zcur_y; y++) {
        for (uint32_t x = zcur_x; x < zcur_x + w_char*nn; x++) {
		   fb_addr[y * fb_width + x] = fb_color;
        }
    }


} pp(Blanks)


void  InitStringSet()
{ 	*ptib=(sCell)initScript;
	ntib=strlen(initScript);
    *v2in = 0;
} pp(InitStringSet)

void  KeyQ()
{ *--Stack= Tos;
	nvm_scheduler_tick();
	Tos = inb(KEYBOARD_STATUS_PORT)&1;



} pp(KeyQ)

void ScanKey()
{ *--Stack= Tos;
   while( !(inb(KEYBOARD_STATUS_PORT) & 0x01) );
   while( inb(KEYBOARD_STATUS_PORT) & 0x01 )
   Tos = inb(KEYBOARD_DATA_PORT);
} pp(ScanKey)

static const char scancode_to_ascii[] = {
    0,   27,  '1', '2', '3',  '4', '5', '6', '7', '8', '9', '0', '-',  '=', '\b',
    '\t', 'q', 'w', 'e', 'r',  't', 'y', 'u', 'i', 'o', 'p', '[', ']',  '\n',
    0,    'a', 's', 'd', 'f',  'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',  0,
    '*',  0,   ' '
};

static const char scancode_to_ascii_shifted[] = {
    0,   27,  '!', '@', '#',  '$', '%', '^', '&', '*', '(', ')', '_',  '+', '\b',
    '\t', 'Q', 'W', 'E', 'R',  'T', 'Y', 'U', 'I', 'O', 'P', '{', '}',  '\n',
    0,    'A', 'S', 'D', 'F',  'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0,    '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',  0,
    '*',  0,   ' '
};

static bool shift_pressed = false;
static bool caps_lock = false;
static bool ctrl_pressed = false;
static bool alt_pressed = false;

bool special_char(int8_t scancode)
{
	    LOG_INFO("scancode=%x\n",scancode);
    if (scancode & 0x80) {
        scancode &= 0x7F;

        if (scancode == 0x2A || scancode == 0x36) {
            shift_pressed = false;
        }

        if (scancode == 0x1D) {
            ctrl_pressed = false;
        }

        if (scancode == 0x38) {
            alt_pressed = false;
        }

        return 1;
    }

    if (scancode == 0x2A || scancode == 0x36) {
        shift_pressed = true;
        return 1;
    }

    if (scancode == 0x1D) {
        ctrl_pressed = true;
        return 1;
    }

    if (scancode == 0x38) {
        alt_pressed = true;
        return 1;
    }

    if (scancode == 0x3A) {
        caps_lock = !caps_lock;
        return 1;
    }

    return 0;

}

void  Key()
{
	Cursor();
	Dup();
    do{ Drop(); ScanKey(); }while(special_char(Tos));
	Cursor();

    if (Tos == 0x4A) { Tos = '-'; return; }
    if (Tos == 0x4E) { Tos = '+'; return; }

    if (Tos >= sizeof(scancode_to_ascii))
	{	Tos <<= 16;
		Tos |= ctrl_pressed ? 0x200 : 0;
		Tos |= alt_pressed ? 0x400 : 0;
		return;}

    if (shift_pressed) {
        Tos = scancode_to_ascii_shifted[Tos];
    } else {
        Tos = scancode_to_ascii[Tos];

        if (caps_lock && Tos >= 'a' && Tos <= 'z') {
            Tos = Tos - 'a' + 'A';
        }
    }
    sCell downTos = Tos | 0x20;
    if (ctrl_pressed && downTos >= 'a' && downTos <= 'z')
        Tos = downTos - 'a' + 1;

} pp(Key)


void Scan2ascii()
{ 

   Tos = (Cell)scancode_to_ascii[(char)Tos];
} pp(Scan2ascii)


void  MakeImag(void)
{
 serial_print("MakeImag run\n");
     kprint("MakeImag run\n",7);
    FthItem("NOOP",pNoop );
 serial_print("NOOP\n");
    FthItem("+",pAdd );
    FthItem("-",pSub );
    FthItem("D+",pDAdd );
    FthItem("1+",pi1Add );
    FthItem("1-",pi1Sub );
    FthItem("2+",pi2Add );
    FthItem("2-",pi2Sub );
    FthItem("INVERT",pInvert);
    FthItem("NEGATE",pNegate);
    FthItem("DNEGATE",pDNegate);
    FthItem("DABS",pDAbs);
    FthItem("*",pMul);
    FthItem("/",pDiv);
    FthItem("2*",pi2Mul);
    FthItem("2/",pi2Div);
    FthItem("MOD",pMod);
    FthItem("U*",pUMul);
    FthItem("U/",pUDiv);
    FthItem("UM*",pUMMul);
    FthItem("UM/MOD",pUMMOD);
    FthItem("/MOD",pDIVMOD);
    FthItem("AND",pAnd);
    FthItem("ANDC",pAndC);
    FthItem("OR",pOr);
    FthItem("XOR",pXor);
    FthItem("ARSHIFT",pARshift);
    FthItem("RSHIFT",pRshift);
    FthItem("LSHIFT",pLshift);
    FthItem("DUP",pDup );
    FthItem("CS-DUP",pDup );
    FthItem("?DUP",pQDup );
    FthItem("OVER",pOver );
    FthItem("CS-OVER",pOver );
    FthItem("TUCK",pTuck );
    FthItem("PICK",pPick );
    FthItem("CS-PICK",pPick );
    FthItem("SWAP",pSwap );
    FthItem("CS-SWAP",pSwap );
    FthItem("2SWAP",pi2Swap );
    FthItem("ROT",pRot );
    FthItem("DROP",pDrop );
    FthItem("NIP",pNip );
    FthItem("2DROP",pi2drop );
    FthItem("2DUP",pi2dup );
    FthItem("2OVER",pi2over);
    FthItem(".",pDot);
    FthItem("U.",pUDot);
    FthItem("THROW",pFThrow);
    FthItem("[",pIMode); Immediate();
    FthItem("]",pCMode);
    FthItem("@",pLoad);
    FthItem("C@",pCLoad);
    FthItem("C!",pCStore);
    FthItem("C+!",pCAddStore);
    FthItem("C!A",pCStoreA);
    FthItem("W@",pWLoad);
    FthItem("W!",pWStore);
    FthItem("L@",pLLoad);
    FthItem("L!",pLStore);
    FthItem("2!",pi2Store);
    FthItem("2@",pi2Load);
    FthItem("COUNT",pCount);
    FthItem("!",pStore);
    FthItem("+!",pAddStore);
    FthItem("1+!",pIncr);
    FthItem("0!",pOff);
    FthItem("OFF",pOff);
    FthItem("ON",pOn);
    FthItem("=",pEqual);
    FthItem("<>",pNEqual);
    FthItem("0<",pZLess);
    FthItem("0=",pZEqual);
    FthItem("0<>",pZNEqual);
    FthItem("D0=",pDZEqual);
    FthItem("<",pLess);
    FthItem(">",pGreat);
    FthItem("U<",pULess);
    FthItem("U>",pUGreat);
    FthItem("MAX",pMax);
    FthItem("MIN",pMin);
    FthItem("0MAX",pi0Max);
    FthItem(">R",pToR);
    FthItem("R>",pFromR);
    FthItem("RDROP",pRDrop);
    FthItem("R@",pRLoad);
    FthItem("2>R",pi2ToR);
    FthItem("2R>",pi2FromR);
    FthItem("2R@",pi2RLoad);
    FthItem("RP@",pRLoad);
    FthItem("RP@",pRPGet);
    FthItem("SP@",pSPGet);
    FthItem("RP!",pRPSet);
    FthItem("SP!",pSPSet);
    FthItem(",",pCompile);
    FthItem("ALLOT",pAllot);
    FthItem("$,",pStrCmp);
    FthItem("<$>",pStr);
    FthItem("Z,",pZStrCmp);
    FthItem("<Z>",pZStr);
    FthItem("EXECUTE",pExecute);
    FthItem("SMUDGE",pSmudge);
    FthItem("TYPE",pType);
    FthItem("ZTYPE",pZType);
    FthItem("CR",pCr);
    FthItem("SPACE",pSpace);
    FthItem("SPACES",pSpaces);
    FthItem("EMIT",pEmit);
    FthItem("PARSE-NAME",pParseName);
    FthItem("PARSE",pParse);
    FthItem("SHEADER",pSHeader);
    FthItem("BUILD",pBuild);
    FthItem("SFIND",pSFind);
    FthItem("SEARCH-WORDLIST",pSearchWordList);
    FthItem("UCOMPARE",pUCompare);
    FthItem("FILL",pFill);
    FthItem("CMOVE",pCmove);
    FthItem("CMOVE>",pCmove_up);
    FthItem("ZCOUNT",pZCount);
    FthItem("HI",pHello);
    FthItem("LOGINFO",pLogInfo);
    FthItem("LOGINFON",pLogInfoN);

    FthItem("KEY?",pKeyQ);
    sCell PKey = Header("KEY");  Co2(pDoDefer,pKey);
    Constant("&SHIFT",(sCell)&shift_pressed);

    FthItem("SCANKEY",pScanKey);
    Constant("CURSOR%",(sCell)&CursorHSize);

    FthItem("S2A",pScan2ascii);


    FthItem("CURSOR",pCursor);
    Constant("CURSOR%",(sCell)&CursorHSize);

/*
    FthItem("SHIFT?",pQShift);
    FthItem("CTL?",pQCtrl);
    FthItem("ALT?",pQAlt);
*/

    FthItem("IMMEDIATE",pImmediate);
    FthItem(":",pColon);
    FthItem(";",pSemicolon);   Immediate();
    FthItem("IF",pIf);         Immediate();
    FthItem("ELSE",pElse);     Immediate();
    FthItem("THEN",pThen);     Immediate();
    FthItem("BEGIN",pBegin);   Immediate();
    FthItem("UNTIL",pUntil);   Immediate();
    FthItem("AGAIN",pAgain);   Immediate();
    FthItem("WHILE",pWhile);   Immediate();
    FthItem("REPEAT",pRepeat); Immediate();

    sCell PTrue = Constant("TRUE",-1);
 serial_print("TRUE\n");

    sCell PCatch = Header("CATCH");  Co2(pto_catch,pfrom_catch);

    FthItem("EXIT",pExit );
    Constant("STATE",(sCell) &State );
    FthItem("?STATE",pStateQ);

    Constant("DOVAR",pDoVar );
    Constant("DOCONST",pDoConst );
    Constant("DODEFER",pDoDefer );
    Constant("DP", (sCell)&here );
    Constant("LAST", (sCell)&Last );
    Constant("LASTCFA", (sCell)&LastCFA );
    VVariable("WARNING",-1);
    FthItem("HERE",pHere);
    Constant("BL",(sCell)' ' );
    sCell PCell = Constant("CELL",sizeof(Cell) );

    FthItem("NAME>",pFromName);
    Constant("BASE",(sCell)&numericBase);

    Header("'");   Co5(pParseName,pSFind,pZEqual,pFThrow,pExit);

    Constant("STATE",(sCell) &State );
    sCell PHi = Header("HI"); Tp("Hello!!!"); Co(pExit);
    sCell PLastin = Constant("LASTIN", (sCell)&Lastin );
    sCell PSaveErrQ = Constant("SAVEERR?", (sCell)&SaveErrQ );

    FthItem("SAVEERR0",pSaveErr0);
    sCell PSaveErr = Header("SAVEERR");  Co2(pDoDefer,pSaveErr0);
    FthItem("PRINTERR0",pPrintErr0);

    sCell PContext = Constant("CONTEXT",(sCell) &Context );
    Constant("CURRENT",(sCell) &Current );
    Constant("IMAGE-BEGIN",(sCell)HereArea );
    Constant("FORTH-WORDLIST",(sCell) &ForthWordlist );
    Constant("CONTEXT-SIZE",ContextSize );
    sCell PSP0 = VVariable("SP0",(sCell) &StackArea[STACK_SIZE-9] );
 serial_print("SP0\n");

    FthItem("R/O",preadOnly);
    FthItem("R/W",preadWrite);
    FthItem("W/O",pwriteOnly);

    FthItem("OPEN-FILE",popenFile);
    FthItem("READ-FILE",preadFile);
    FthItem("READ-LINE",preadLine);
    FthItem("WRITE-FILE",pwriteFile);


    FthItem("CLOSE-FILE",pcloseFile);

    FthItem("OPEN-DIR",pOpenDir);
    FthItem("DIRI2NAME",pDirI2Name);
    FthItem("DIRI2TYPE",pDirI2Type);
    FthItem("CLOSE-DIR",pCloseDir);
    Constant("G_CLI_PATH",(sCell)current_working_directory);
    Constant("VFS_TYPE_DIR",VFS_TYPE_DIR);


    Constant("ENTTT",(sCell)&entries);

    sCell Pi2in = VVariable(">IN",0);
    v2in = (sCell*)(Pi2in+sizeof(Cell));

    sCell PATib = Constant("ATIB",(sCell)&atib);
    sCell Pntib = Constant("#TIB",(sCell)&ntib);
    sCell Ptib = Constant("TIB",(sCell)&ptib);
    ptib = (sCell*)(Ptib+sizeof(Cell));

    FthItem("SOURCE",pSource);
    FthItem("SOURCE!",pSourceSet);

    sCell PSourceId = Constant("SOURCE-ID", 0);

    FthItem("ALLOCATE",pAllocate);
    FthItem("FREE",pFree);

    Constant("YDP", (sCell)&YDP);
    Constant("YDP0", (sCell)&YDP0);
    FthItem("YDP_FL",pYDPFL);

	FthItem("SETXY",pSetXY);
	FthItem("GETXY",pGetXY);

    Constant("&COLOR", (sCell)&text_color);

    sCell PErrDO1 = Header("ERROR_DO1");	Co3(PSaveErr,pPrintErr0,pExit);
    sCell PErrDO = Header("ERROR_DO");  Co2(pDoDefer,PErrDO1);

    sCell PAccept = Header("ACCEPT");  Co2(pDoDefer,pAccept);
    sCell PQuery = Header("QUERY");
    Co4(Ptib,pLit_,256,PAccept);
//    Co6(Ptib,pDup,pUDot,pLit_,256,PAccept);
    Co5(Pntib,pStore,Pi2in,pOff,pExit);


//    FthItem("QUERY",pQuery);

    sCell PBye = Header("BYE"); Co2(pBye,pExit);
 serial_print("BYE\n");

    sCell PLitC = Header("LIT,");  Co2(pDoDefer,pLitCo);
    sCell PPre = Header("<PRE>");  Co2(pDoDefer,pNoop);
    sCell PFileRefill = Header("FREFILL");  Co2(pDoDefer,pNoop);
    sCell PQStack = Header("?STACK");  Co2(pDoDefer,pNoop);

    sCell PRefill = Header("REFILL");
	Co(PSourceId);
    If();   Co2(PFileRefill,pDup);  If(); Co(PPre); Then();
 serial_print("IF\n");
    Else(); Co2(PQuery,PTrue);
 serial_print("Else\n");
    Then(); Co(pExit);
 serial_print("Then\n");

    FthItem("SNUMBER0",pSNumber0);

    sCell PSNumber = Header("SNUMBER");  Co2(pDoDefer,pSNumber0 );

    sCell PQSLiteral0 = Header("?SLITERAL0");
	Co(PSNumber);
	If(); Lit(-13); Co(pFThrow);
	Else(); Co(pStateQ); If(); Co(PLitC); Then();
	Then();
    Co(pExit);

    sCell PQSLiteral = Header("?SLITERAL");
    Co2(pDoDefer,PQSLiteral0);

    sCell PInterpret1 = Header("INTERPRET1");
    Begin();
        Co6(Pi2in,pLoad,PLastin,pStore,PSaveErrQ,pOn);
        Co2(pParseName,pDup);
//        Co4(pParseName,pi2dup,pType,pDup);
    While();  Co2(pSFind,pQDup);
        If();
            Co2(pStateQ,pEqual);
            If();   Co(pCompile );
            Else(); Co(pExecute );
            Then();
        Else(); Co(PQSLiteral);
        Then(); Co(PQStack);
    Repeat();
    Co2(pi2drop,pExit);

    sCell PInterpret = Header("INTERPRET");
    Co2(pDoDefer,PInterpret1 );

    sCell PQuit = Header("QUIT");
    Begin();	Co(PRefill); 
    While();	Co(PInterpret);	Tp(" ok\n>");
    Repeat();	Co(pExit);

    sCell PWords = Header("WORDS");
    Co3(PContext,pLoad,pLoad);
    Begin(); Co(pDup);
    While(); Co7(pDup,pCount,pType,pSpace,PCell,pSub,pLoad );
    Repeat(); Co2(pDrop,pExit );

    ip = here;  // SYS START

	Tp("Forth\n");

//    Co(PWords);
    Co(pCr);
    Co(pInitStringSet);
    Co5(pIMode,pLit_,PInterpret,PCatch,pQDup );
    If(); Co5(PErrDO,PSP0,pLoad,pSPSet,pCr ) ;
    Then();

    Begin();
	Co4(PATib,pLit_,Ptib+sizeof(Cell),pStore);
        Co5(pIMode,pLit_,PQuit,PCatch,PErrDO);
        Co4(PSP0,pLoad,pSPSet,pCr ) ;
    Again();
}

bool forth_init = true ;

void cmd_forth(int argc, char* argv[])
{
     if(forth_init) {
	forth_init = false ;
	HereArea = kmalloc(sizeof(sCell)*HERE_SIZE1);
	if(!HereArea) { kprint("HereArea kmalloc error\n", 4); return; }
        LOG_TRACE("HereArea = %p\n",HereArea);
        LOG_TRACE("HereArea = %p\n",&HereArea[HERE_SIZE1]);
//	HereArea[HERE_SIZE1]=0x7777777777;
	StackArea = kmalloc(sizeof(sCell)*STACK_SIZE);
	if(!HereArea) { kprint("StackArea kmalloc error\n", 4); return; }
	RStackArea = kmalloc(sizeof(sCell)*RSTACK_SIZE);
	if(!HereArea) { kprint("RStackArea kmalloc error\n", 4); return; }

    entries = kmalloc(32 * sizeof(vfs_dirent_t));
    if (!entries) { 
        kprint("ls: out of memory\n", 7); 
        return; 
    }
    
	SignPrim=pNoop&SIGN_PRIM_MASK;

	LOG_INFO("Hello from Forth!!!\n");

	memset(input_buffer, 0, 256);


	here = HereArea ;
	Stack = &StackArea[STACK_SIZE-8] ;
	rStack = &RStackArea[RSTACK_SIZE-8] ;

        ForthWordlist[0] = 0;
        ForthWordlist[1] = 0;
        ForthWordlist[2] = 0;

	Context[0] = ForthWordlist;
	Context[1] = 0;
	Current[0] = ForthWordlist;
	ireg = (sCell)MakeImag;
 serial_print("~>"); serial_hex64(ireg); serial_print("->"); serial_hex64((sCell)MakeImag);    serial_print("\n");


   kprint("Hello from Forth!!!\n", 5);
 }
	forth_run=1;

      while (forth_run)
      {   do{
// serial_print(">"); serial_hex64((Cell)ip); serial_print(": "); serial_hex64(ireg);    serial_print("\n");
              ((proc) (ireg))();
              ireg = *ip++;
          }while ( (ireg&SIGN_PRIM_MASK) == SignPrim );
          do{
// serial_print("|>"); serial_hex64((Cell)ip); serial_print(": "); serial_hex64(ireg);  serial_print("\n");
              *--rStack = (sCell) ip;  ip =  (sCell *) ireg;
              ireg = *ip++;
          }while ( (ireg&SIGN_PRIM_MASK) != SignPrim );
      }

//	kfree(HereArea);
//	kfree(StackArea);
//	kfree(RStackArea);

//   kprint("bye from Forth!!!\n", 6);

}
