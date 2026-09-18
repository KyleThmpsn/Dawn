"""Verify or regenerate opcode-701 wire fixtures from the supported native image.

Read-only by default. Pass --write to regenerate the C++ descriptor and fixtures.
The independent Python codec uses native reflection, not the C++ implementation.
"""
from pathlib import Path
import argparse
import hashlib
import re
import struct

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('image', type=Path, help='Mapped destiny2_unpacked.bin from build 86657')
parser.add_argument('--write', action='store_true')
args = parser.parse_args()
B = args.image.read_bytes()
assert hashlib.sha256(B).hexdigest() == '63d128f1c759b92d32b0f226bcbec828bc58cef193ee0df6fdd582bd0290ed1e'
ROOT = Path(__file__).resolve().parents[2]

def publish(path, text):
    if args.write:
        path.write_text(text)
    else:
        assert path.read_text() == text, f'Native schema differs: {path}'
    print(f'PASS {path.relative_to(ROOT)}')

def u32(p):return struct.unpack_from('<I',B,p)[0]
def u64(p):return struct.unpack_from('<Q',B,p)[0]
cache={}
def desc(tag):
    if tag in cache:return cache[tag]
    candidates=[]
    for m in re.finditer(re.escape(struct.pack('<I',tag)),B[0x371e000:0x3ba2000]):
        p=m.start()+0x371e000
        for delta in (0x68,0x70):
            a=p-delta
            if u32(p-16)==0x80800050 and u32(p-12)==0x8080010a and u32(a+0x14)==u32(p+4) and 0<u32(p-8)<1000:
                candidates.append((a,p))
    assert len(candidates)==1,(hex(tag),candidates)
    a,p=candidates[0];rows=[]
    for i in range(u32(p-8)):
        raw=struct.unpack_from('<10I',B,p+0x18+i*0x28)
        rows.append(dict(offset=raw[0],kind=raw[4]&255,optional=bool(raw[4]&256),child=hex(raw[5]),bias=raw[6],width=raw[7],raw=raw))
    out=dict(tag=hex(tag),rva=hex(a),size=u32(a+0x14),array=u32(p+16),header=B[a:a+0x60].hex(),fields=rows)
    cache[tag]=out
    return out
def tree(tag):
    d=desc(tag)
    for f in d['fields']:
        if f['kind']==1:tree(int(f['child'],16))
    return cache

tree(0x80807603)
lines=['// Pinned build 86657, opcode 701 (80807603), reflected from the native descriptors.',
       '// Offsets are within its 0x9C0 account-update body, starting at account + 0x748.',
       '// Fixed arrays retain per-element presence bits. Unrelated fields are consumed only.']
done=set()
def emit(tag):
    if tag in done:return
    d=desc(tag)
    for f in d['fields']:
        if f['kind']==1:emit(int(f['child'],16))
    lines.append(f'constexpr std::array<Field, {len(d["fields"])}> schema{tag:08X}{{{{')
    for f in d['fields']:
        kind=f['kind'];width=1 if kind==2 else f['raw'][8] if kind==11 else f['width']
        size={1:0,2:1,3:1,4:2,5:4,6:8,8:2,9:4,10:8,11:4}[kind]
        child='schema'+f['child'][2:].upper() if kind==1 else '{}'
        offset=0 if d['array'] else f['offset'];count=d['array'] or 1;stride=f['offset'] if d['array'] else 0
        lines.append(f'    {{{offset}, {count}, {stride}, {width}, {size}, {str(f["optional"]).lower()}, 0x{f["bias"]:08X}U, {child}}},')
    lines.append('}};');done.add(tag)
emit(0x80807603)
publish(ROOT/'Dawn/src/middleware/web_service/messages/opcode701_schema.inl', '\n'.join(lines)+'\n')

def encode(data,selected=None):
    bits=''
    def put(n,w):
        nonlocal bits
        bits+=format(n&((1<<w)-1),f'0{w}b')
    def selected_in(tag,base):
        d=desc(tag)
        return selected is None or any(base<=p<base+d['size'] for p in selected)
    def field(f,base):
        if f['optional']:
            present=selected_in(int(f['child'],16),base) if f['kind']==1 else selected is None or base in selected
            put(present,1)
            if not present:return
        if f['kind']==1:walk(int(f['child'],16),base);return
        size={2:1,3:1,4:2,5:4,6:8,8:2,9:4,10:8,11:4}[f['kind']]
        value=int.from_bytes(data[base:base+size],'little',signed=f['kind'] in (3,4,5,6))
        width=1 if f['kind']==2 else f['raw'][8] if f['kind']==11 else f['width']
        put(value+f['bias'],width)
    def walk(tag,base):
        d=desc(tag)
        if d['array']:
            for i in range(d['array']):field(d['fields'][0],base+i*d['fields'][0]['offset'])
        else:
            for f in d['fields']:field(f,base+f['offset'])
    walk(0x80807603,0)
    put(0,2);bits+='0'*((-len(bits))%8)
    return int(bits,2).to_bytes(len(bits)//8,'big')


data=bytearray(0x9c0)
def setv(offset,fmt,value):struct.pack_into('<'+fmt,data,offset,value)
p=0x44c;k=0x6f8
setv(p,'i',1);setv(p+8,'i',20);setv(p+12,'f',1.25)
setv(p+31,'b',8);setv(p+32,'b',7);setv(p+33,'b',6);setv(p+34,'b',3)
setv(p+64,'f',10000);setv(p+68,'f',0.5)
setv(p+74,'B',1);setv(p+76,'B',1)
setv(k,'i',1);setv(k+4,'B',1);setv(k+5,'B',2);setv(k+8,'i',30)
for i in range(60):setv(k+16+i*4,'I',0x00740074)
setv(k+16+20*4,'I',0x006F0039) # native jump: space / right mouse
setv(k+16+58*4,'I',0x00740101) # modifier bit survives round-trip
vectors={'full':encode(data),'empty':encode(data,set())}
setv(p+8,'i',17);setv(p+34,'b',5);setv(k+8,'i',20);setv(k+12,'B',1);setv(k+5,'B',0)
vectors['partial']=encode(data,{p+8,p+34,k+5,k+8,k+12})
setv(p+8,'i',0);vectors['invalid_mouse']=encode(data,{p+8})
out=['// Generated independently from pinned native 80807603 reflection (build 86657).',
     '#pragma once','#include <array>','#include <cstddef>','namespace settings_wire_fixture {']
for name,b in vectors.items():
 out.append(f'inline constexpr std::array<std::byte, {len(b)}> {name}{{{{')
 for i in range(0,len(b),16):out.append('    '+','.join(f'std::byte{{0x{x:02X}}}' for x in b[i:i+16])+',')
 out.append('}};')
out.append('}')
publish(ROOT/'Dawn/unit/fixtures/account_settings_wire.h', '\n'.join(out)+'\n')
