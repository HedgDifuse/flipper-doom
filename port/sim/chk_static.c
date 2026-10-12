#include <stdio.h>
#include "doomdef.h"
#include "info.h"
#include "p_mobj.h"
int main(void){
  int n=0;
  for(int t=MT_MISC0;t<NUMMOBJTYPES;t++){
    long f=mobjinfo[t].flags;
    if(f&(MF_SHOOTABLE|MF_MISSILE|MF_COUNTKILL|MF_PICKUP|MF_NOCLIP)) printf("type %d flags %lx\n",t,f);
    n++;
  }
  printf("%d static types from %d (MT_MISC0) of %d\n", n, MT_MISC0, NUMMOBJTYPES);
  for(int t=0;t<MT_MISC0;t++) printf("%d:%ld ", t, mobjinfo[t].doomednum); printf("\n");
}
