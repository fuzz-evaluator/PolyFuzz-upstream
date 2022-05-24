
import sys
import atheris
import urllib3

import pyprob

pyprob.Setup('py_summary.xml', 'driver.py')

def LoadBytes (FName):
    bytes = None
    with open (FName, "rb") as bf:
        bytes = bf.read()
    return bytes

if __name__ == "__main__":
    try:
        data = LoadBytes (sys.argv[1])
        
        fdp = atheris.FuzzedDataProvider(data)
        original = fdp.ConsumeUnicode(sys.maxsize)
    
        urllib3.util.parse_url(original)
    except Exception as e:
        #print (e)
        pyprob.PyExcept (type(e).__name__, __file__, e.__traceback__.tb_lineno)

