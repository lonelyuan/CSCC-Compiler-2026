// Experiment 1b: separate "too many bytes" from "bytes in the wrong places".
//
// No runtime, no scheduler, no barriers, no locks: each thread owns a disjoint
// set of C blocks and runs straight through them. So any change in per-thread
// throughput between T=1 and T=40 is the memory system alone.
//
// Four variants, all calling the OFFICIAL madd, same total madd count:
//   strided  : task = fixed (row,col), stream k.  What ships today. 2 tiles/madd.
//   packed   : same traversal, but tiles copied into contiguous b*b buffers so
//              the official madd is called with lda=b instead of lda=n.
//   tiled    : task = RxR block of (row,col) sharing one k. 2/R tiles/madd.
//   tiled+pk : both.
#include "kernels.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <vector>
#include <string>
#include <algorithm>

static int n, b, B, NK, R, NT;
static std::vector<double> L, C;
static inline double *Lt(int i,int k){ return &L[(size_t)i*b*n + (size_t)k*b]; }
static inline double *Ct(int i,int j){ return &C[(size_t)i*b*n + (size_t)j*b]; }
static void packin(double *dst, const double *src){
    for(int r=0;r<b;++r) std::memcpy(dst+(size_t)r*b, src+(size_t)r*n, sizeof(double)*b);
}
static void packout(double *dst, const double *src){
    for(int r=0;r<b;++r) std::memcpy(dst+(size_t)r*n, src+(size_t)r*b, sizeof(double)*b);
}

// Rows are split across threads; each thread owns whole row-blocks of C.
static void work(int tid, int mode){
    const int nrb = (B + R - 1) / R;
    std::vector<double> pA((size_t)R*b*b), pB((size_t)R*b*b), pC((size_t)R*R*b*b);
    for(int rb = tid; rb < nrb; rb += NT){
        const int r0 = rb*R, r1 = std::min(B, r0+R);
        for(int c0 = 0; c0 < B; c0 += R){
            const int c1 = std::min(B, c0+R);
            if(mode==0){ // strided: per (row,col), stream k
                for(int i=r0;i<r1;++i) for(int j=c0;j<c1;++j)
                    for(int k=0;k<NK;++k) madd(Lt(i,k), Lt(j,k), Ct(i,j), b, n);
            } else if(mode==1){ // packed, same traversal
                for(int i=r0;i<r1;++i) for(int j=c0;j<c1;++j){
                    packin(&pC[0], Ct(i,j));
                    for(int k=0;k<NK;++k){
                        packin(&pA[0], Lt(i,k)); packin(&pB[0], Lt(j,k));
                        madd(&pA[0], &pB[0], &pC[0], b, b);
                    }
                    packout(Ct(i,j), &pC[0]);
                }
            } else if(mode==2){ // 2D tiled, lda=n
                for(int k=0;k<NK;++k)
                    for(int i=r0;i<r1;++i) for(int j=c0;j<c1;++j)
                        madd(Lt(i,k), Lt(j,k), Ct(i,j), b, n);
            } else { // 2D tiled + packed
                for(int i=r0;i<r1;++i) for(int j=c0;j<c1;++j)
                    packin(&pC[((size_t)(i-r0)*R+(j-c0))*b*b], Ct(i,j));
                for(int k=0;k<NK;++k){
                    for(int i=r0;i<r1;++i) packin(&pA[(size_t)(i-r0)*b*b], Lt(i,k));
                    for(int j=c0;j<c1;++j) packin(&pB[(size_t)(j-c0)*b*b], Lt(j,k));
                    for(int i=r0;i<r1;++i) for(int j=c0;j<c1;++j)
                        madd(&pA[(size_t)(i-r0)*b*b], &pB[(size_t)(j-c0)*b*b],
                             &pC[((size_t)(i-r0)*R+(j-c0))*b*b], b, b);
                }
                for(int i=r0;i<r1;++i) for(int j=c0;j<c1;++j)
                    packout(Ct(i,j), &pC[((size_t)(i-r0)*R+(j-c0))*b*b]);
            }
        }
    }
}

int main(int argc,char**argv){
    n=atoi(argv[1]); b=atoi(argv[2]); NK=atoi(argv[3]); R=atoi(argv[4]);
    const char *modes[4]={"strided","packed","tiled","tiled+pk"};
    B=n/b;
    L.assign((size_t)n*n,0.0); C.assign((size_t)n*n,0.0);
    for(size_t i=0;i<L.size();++i) L[i]=1.0/(double)(1+(i%97));
    printf("n=%d b=%d B=%d NK=%d R=%d  madds=%lld  intensity=%.2f flop/byte\n",
           n,b,B,NK,R,(long long)B*B*NK, b/12.0);
    printf("%-9s %-5s %-11s %-12s %-12s\n","variant","T","wall_s","perthread_GF","aggregate_GF");
    const double fl = 2.0*(double)b*b*b*(double)B*B*NK;
    for(int mode=0;mode<4;++mode){
        double base=0;
        for(int T : {1,40}){
            NT=T;
            std::memset(&C[0],0,C.size()*sizeof(double));
            auto s=std::chrono::steady_clock::now();
            std::vector<std::thread> th;
            for(int i=1;i<T;++i) th.emplace_back(work,i,mode);
            work(0,mode);
            for(auto&x:th) x.join();
            double w=std::chrono::duration<double>(std::chrono::steady_clock::now()-s).count();
            double per=fl/w/1e9/T, agg=fl/w/1e9;
            if(T==1) base=per;
            char note[32]; note[0]='\0';
            if(T!=1) std::snprintf(note,sizeof note,"perthread %+.0f%%",100*per/base-100);
            printf("%-9s %-5d %-11.6f %-12.2f %-12.2f %s\n",modes[mode],T,w,per,agg,note);
        }
    }
    return 0;
}
