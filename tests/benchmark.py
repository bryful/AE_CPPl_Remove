import pathlib, statistics, subprocess, tempfile, time
root=pathlib.Path(__file__).resolve().parent
sample=root.parent/'sample/AAA.aep'
for exe in [root/'delCppl_before.exe',root.parent/'x64/Release/delCppl.exe']:
 if not exe.exists():continue
 for patch in [False,True]:
  times=[]
  with tempfile.TemporaryDirectory(prefix='bench_',dir=root) as tmp:
   folder=pathlib.Path(tmp);assert folder.resolve().parent==root
   src=folder/'AAA.aep';src.write_bytes(sample.read_bytes())
   for i in range(8):
    t=time.perf_counter()
    r=subprocess.run([str(exe),str(src)]+(['-p'] if patch else []),capture_output=True)
    assert r.returncode==0,r.stderr
    times.append((time.perf_counter()-t)*1000)
    for output in folder.glob('*_patched*.aep'):output.unlink()
  print(exe.name,'patch' if patch else 'info',round(statistics.median(times[1:]),2),'ms')
