import re,sys
s=open(sys.argv[1]).read()
s=s.replace('#include <metal_stdlib>','').replace('using namespace metal;','')
s=re.sub(r'\b(float2|float3|float4|uint2)\(', r'mk_\1(', s)
s=re.sub(r'(?<![\w.])(\d+\.\d*)(?![\w.])', r'\1f', s)   # MSL literals are float
open(sys.argv[2],'w').write(s)
s=open(sys.argv[2]).read()
s=re.sub(r'(?<![<\w])(texture2d<\w+>)\s+(\w+)(?=\s*[,)\[])', r'const \1 &\2', s)
s=s.replace('constant NtscParams &p','const NtscParams &p').replace('constant OutParams &u','const OutParams &u')
open(sys.argv[2],'w').write(s)
