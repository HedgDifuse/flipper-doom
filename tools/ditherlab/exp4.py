import numpy as np
from PIL import Image
import ditherlab as D, aware as R
data=open(D.SRC,'rb').read(); D.algos(); BN=D.BN; B4=D.bayer(4)
def localthr(l):
    m=R.boxblur(l,6); return np.clip(0.5+2*(l-m)+0.35*(m-l.mean()),0,1)>0.5
V={
 'E b4 3lvl+spr': lambda l,ids: R.render(l,ids,B4,levels=3),
 'F light 3lvl': lambda l,ids: R.render(l,ids,B4,levels=3,lo=0.45,hi=1.0),
 'G light+floor': lambda l,ids: R.render(l,ids,B4,levels=3,lo=0.45,hi=1.0,plane_lift=0.35),
 'H light 4lvl bn': lambda l,ids: R.render(l,ids,BN,levels=4,lo=0.4,hi=1.0,plane_lift=0.2),
 'I light 5lvl b4': lambda l,ids: R.render(l,ids,B4,levels=5,lo=0.35,hi=1.0,plane_lift=0.2),
}
rows=[]
for i in (900,1800,2700,4500,3600):
    rgb=D.load_frame(data,i); l=D.tone(D.to_view(rgb)); ids=R.load_ids(i)
    ims=[Image.fromarray(rgb[8:168]).resize((384,192))]+[D.lcd(f(l,ids),3) for f in V.values()]
    row=Image.new('RGB',(388*len(ims),192),(30,30,30))
    for k,im in enumerate(ims): row.paste(im,(k*388,0))
    rows.append(row)
s=Image.new('RGB',(rows[0].width,200*len(rows)),(30,30,30))
for k,r in enumerate(rows): s.paste(r,(0,k*200))
s.save('out/exp4.png')
s.crop((0,0,388*6,400)).save('out/exp4_a.png'); s.crop((0,400,388*6,1000)).save('out/exp4_b.png')
