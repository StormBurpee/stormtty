#!/usr/bin/env python3
"""Linux PTY source-cost comparison only; not a native Windows claim."""
import subprocess,time,statistics,json,pty,os
from pathlib import Path
import argparse
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--overlay',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
a=parser.parse_args()
root=a.overlay.resolve(strict=True)
base='autoload -Uz compinit; compinit -i -D; '
cmd='STORMTTY_EPHEMERAL=0; _stormtty_state=/nonexistent; source "$STORMTTY_BENCH_ROOT/stormtty/vendor/shell-tools/init.zsh"; '
samples=[[],[]]
for i in range(60):
 for k,s in enumerate([base,base+cmd]):
  master,slave=pty.openpty(); t=time.perf_counter()
  p=subprocess.run(['zsh','-fic',s+'exit 0'],stdin=slave,stdout=slave,stderr=slave,env=dict(os.environ,TERM='xterm-256color',STORMTTY_BENCH_ROOT=str(root)),timeout=15)
  dt=time.perf_counter()-t; os.close(slave)
  try: output=os.read(master,65536)
  except OSError: output=b''
  os.close(master)
  if p.returncode or b'can\'t' in output: raise RuntimeError(output)
  if i>=10:samples[k].append(dt*1000)
r=dict(host='Linux /usr/bin/zsh with PTY, NOT Windows native; warm cache; compinit -i -D included',n=50,baseline_median_ms=statistics.median(samples[0]),tools_median_ms=statistics.median(samples[1]),paired_overhead_median_ms=statistics.median([b-a for a,b in zip(*samples)]))
print(json.dumps(r,indent=2)); a.output.write_text(json.dumps(r,indent=2)+'\n')
