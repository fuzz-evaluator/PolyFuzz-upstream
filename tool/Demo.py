#!/usr/bin/python
import os
import pyins
from DemoAdd import DemoAdd
    
def DemoTr (Value):
    Var = os.getenv("CASE1")
    if Var == None:
    	Var = 1
    Var = int (Var)
    Da = DemoAdd (Var)
    Res = Da.Add (Value)
    return Res


if __name__ == '__main__':
    pyins.Setup()
    Temp = 8
    Result = DemoTr(Temp)
    print ("trace end", Result)

