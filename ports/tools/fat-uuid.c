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
    if(argc!=2)return 2;
    long fd=call(56,-100,(long)argv[1],0x80000,0);if(fd<0)return 3;
    long n=call(67,fd,(long)block,sizeof(block),0);call(57,fd,0,0,0);
    if(n<512 || block[510]!=0x55 || block[511]!=0xaa)return 4;
    unsigned bps=block[11]|((unsigned)block[12]<<8);
    if(bps<512 || bps>4096 || (bps&(bps-1)))return 5;
    unsigned root_entries=block[17]|((unsigned)block[18]<<8);
    unsigned sectors_fat=block[22]|((unsigned)block[23]<<8);
    unsigned off=sectors_fat && root_entries?39:67;
    if(block[off-1]!=0x29)return 6;
    char out[10];const char hex[]="0123456789abcdef";unsigned p=0;
    for(int i=3;i>=0;i--){unsigned v=block[off+i];out[p++]=hex[v>>4];out[p++]=hex[v&15];if(i==2)out[p++]='-';}
    out[p++]='\n';return call(64,1,(long)out,p,0)==p?0:7;
}
__asm__(".global _start\n_start:\n mov x29, xzr\n mov x30, xzr\n ldr x0, [sp]\n add x1, sp, #8\n bl main\n mov x8, #93\n svc #0\n");
