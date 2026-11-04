"""Replace only these SDK members, under the caller's libapps lock."""
from pathlib import Path
from collections import Counter
import sys,subprocess,shutil,os
ar,target,*libraries=sys.argv[1:];target=Path(target)
def members(path):return subprocess.check_output([ar,'t',str(path)],text=True).splitlines()
owned=[n for lib in libraries for n in members(lib)]
assert len(owned)==len(set(owned)),'SDK member collision'
assert all(Path(n).name==n and '\n' not in n for n in owned)
allowed=set(owned);before=members(target) if target.exists() else []
foreign=Counter(n for n in before if n not in allowed)
temp=target.with_name(target.name+'.box-sr.tmp')
try:
 if target.exists():
  shutil.copy2(target,temp)
  for attempt in range(128):
   present=sorted(set(members(temp))&allowed)
   if not present:break
   subprocess.run([ar,'d',str(temp),*present],check=True,capture_output=True)
  else:raise AssertionError('SDK cleanup did not converge')
  action='open'
 else:action='create'
 script=action+' '+str(temp)+'\n'+''.join('addlib '+p+'\n' for p in libraries)+'save\nend\n'
 subprocess.run([ar,'-M'],input=script,text=True,check=True,capture_output=True)
 subprocess.run([ar,'s',str(temp)],check=True,capture_output=True)
 after=members(temp)
 assert Counter(n for n in after if n not in allowed)==foreign
 assert Counter(n for n in after if n in allowed)==Counter(owned)
 os.replace(temp,target)
 print('BOX SDK merge verified:',len(owned),'SDK members;',sum(foreign.values()),'other members preserved')
finally:
 if temp.exists():temp.unlink()
