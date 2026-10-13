# Copyright (C) 2026 HedgDifuse
# SPDX-License-Identifier: GPL-2.0-or-later
import numpy as np, os
from PIL import Image
import ditherlab as D
data=open(D.SRC,'rb').read(); A=D.algos(); BN=D.BN; B4=D.bayer(4)
def boxblur(l,r):
    k=np.ones(2*r+1)/(2*r+1); p=np.pad(l,r,mode='edge')
    t=np.apply_along_axis(lambda v:np.convolve(v,k,'valid'),1,p)
    return np.apply_along_axis(lambda v:np.convolve(v,k,'valid'),0,t)
def stretch(l):
    lo,hi=np.percentile(l,2),np.percentile(l,98); return np.clip((l-lo)/(hi-lo+1e-3),0,1)
def local(l,r=6,k=2.0,base=0.35):
    m=boxblur(l,r); return np.clip(0.5+k*(l-m)+base*(m-l.mean()),0,1)
def poster(l,levels=5,m=B4):
    q=l*(levels-1); f=q-np.floor(q)
    th=np.tile(m,(16,32))[:64,:128]
    # snap fraction to quantized pattern so only `levels` densities exist
    return (np.floor(q)+ (f>th))/(levels-1) > 0.5 if False else (np.floor(q)+(f>th))/(levels-1)
def patt(v):  # v in {0,.25,.5,.75,1} -> 2x2 pattern bits
    th=np.tile(np.array([[0.125,0.625],[0.875,0.375]]),(32,64)); return v>th
V={
 'raw_threshold':lambda l: l>np.median(l),
 'stretch_bn':lambda l: D.ordered(stretch(l),BN),
 'stretch_b4':lambda l: D.ordered(stretch(l),B4),
 'local_bn':lambda l: D.ordered(local(l),BN),
 'local_b4':lambda l: D.ordered(local(l),B4),
 'local_thr':lambda l: local(l)>0.5,
 'post5_2x2':lambda l: patt(np.round(stretch(l)*4)/4),
 'local_post':lambda l: patt(np.round(local(l,6,1.6,0.5)*4)/4),
}
rows=[]
for i in (900,1800,2700,4500):
    l=D.to_view(D.load_frame(data,i))
    ims=[Image.fromarray(D.load_frame(data,i)[8:168]).resize((384,192))]+[D.lcd(f(l),3) for f in V.values()]
    row=Image.new('RGB',(384*len(ims)+4*len(ims),192),(30,30,30))
    for k,im in enumerate(ims): row.paste(im,(k*388,0))
    rows.append(row)
s=Image.new('RGB',(rows[0].width,200*len(rows)),(30,30,30))
for k,r in enumerate(rows): s.paste(r,(0,k*200))
s.save('out/exp2.png'); print(list(V))
