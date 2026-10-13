// Copyright (C) 2026 HedgDifuse
// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdio.h>
#include "doomdef.h"
#include "info.h"
#include "p_mobj.h"
int main(void){
  for(int t=MT_MISC0;t<NUMMOBJTYPES;t++){
    int st=mobjinfo[t].spawnstate, seen=0;
    for(int k=0;k<64 && st!=S_NULL;k++){
      if(states[st].action) printf("type %d state %d has an action\n",t,st);
      st=states[st].nextstate; if(st==mobjinfo[t].spawnstate) break;
    }
    int d=mobjinfo[t].deathstate; if(d) printf("type %d has deathstate %d\n",t,d);
  }
  puts("done");
}
