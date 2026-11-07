/* Read only an ext superblock UUID. Freestanding AArch64 Linux early boot tool. */
typedef unsigned long usize;
static long call(long n,long a,long b,long c,long d) {
    register long x8 __asm__("x8")=n;
    register long x0 __asm__("x0")=a,x1 __asm__("x1")=b,x2 __asm__("x2")=c,x3 __asm__("x3")=d;
    __asm__ volatile("svc 0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3):"memory");
    return x0;
}
static unsigned char block[4096];
int main(long argc,char **argv) {
    if (argc!=2) return 2;
    long fd=call(56,-100,(long)argv[1],0x80000,0); /* openat O_RDONLY|O_CLOEXEC */
    if(fd<0)return 3;
    long n=call(67,fd,(long)block,sizeof(block),0); /* pread64, no shared offset */
    call(57,fd,0,0,0);
    if(n<2048)return 4;
    if(block[1080]!=0x53 || block[1081]!=0xef)return 5;
    char out[37];const char hex[]="0123456789abcdef";unsigned pos=0,nonzero=0;
    for(unsigned i=0;i<16;i++) {
        if(i==4 || i==6 || i==8 || i==10)out[pos++]='-';
        unsigned v=block[1128+i];nonzero|=v;out[pos++]=hex[v>>4];out[pos++]=hex[v&15];
    }
    if(!nonzero)return 6;
    out[pos++]='\n';return call(64,1,(long)out,pos,0)==pos?0:7;
}
__asm__(".global _start\n_start:\n mov x29, xzr\n mov x30, xzr\n ldr x0, [sp]\n add x1, sp, #8\n bl main\n mov x8, #93\n svc #0\n");
