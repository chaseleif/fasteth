#! /usr/bin/env python

import os

maxsp = 36

q1start = (maxsp+3)//4
q2start = q1start<<1
q3start = q1start+q2start

for i in range(0,maxsp):
    with open('./inputs/input'+str(i),'w') as outfile:
        framenum=1
        if i%6==1:
            waitcount = i%9+1
            outfile.write('Wait for receiving '+str(waitcount)+' frames\n')
        if i>4:
            outfile.write('Frame '+str(framenum)+', To SP '+str(i-3)+'\n')
            framenum+=1
        if i<maxsp-6:
            outfile.write('Frame '+str(framenum)+', To SP '+str(i+4)+'\n')
            framenum+=1
        if i>6 and i%4==0:
            outfile.write('Frame '+str(framenum)+', To SP '+str(i-5)+'\n')
            framenum+=1
        if i<maxsp-9 and i%3==0:
            outfile.write('Frame '+str(framenum)+', To SP '+str(i+7)+' hallo\n')
            framenum+=1
        if i%4==0:
            waitcount = i%7+1
            outfile.write('Wait for receiving '+str(waitcount)+' frames\n')
        while framenum<=10:
            if i>10:
                outfile.write('Frame '+str(framenum)+', To SP '+str(i-framenum)+'\n')
                framenum+=1
            if i<maxsp-12:
                outfile.write('Frame '+str(framenum)+', To SP '+str(i+framenum)+'\n')
                framenum+=1
            if i>0:
                outfile.write('Frame '+str(framenum)+', To SP 0\n')
                framenum+=1
            if i<maxsp-1:
                outfile.write('Frame '+str(framenum)+', To SP '+str(maxsp-1)+'\n')
                framenum+=1
        if i>q1start:
            outfile.write('Frame '+str(framenum)+', To SP '+str(i-q1start)+'\n')
            framenum+=1
        if i>q2start:
            outfile.write('Frame '+str(framenum)+', To SP '+str(i-q2start)+'\n')
            framenum+=1
        if i>q3start:
            outfile.write('Frame '+str(framenum)+', To SP '+str(i-q3start)+'\n')
            framenum+=1
        if i<q1start:
            outfile.write('Frame '+str(framenum)+', To SP '+str(i+q2start)+'\n')
            framenum+=1
        if i<q2start:
            outfile.write('Frame '+str(framenum)+', To SP '+str(i+q1start)+'\n')
            framenum+=1
        if i<q3start:
            outfile.write('Frame '+str(framenum)+', To SP '+str(i+1)+'\n')
