# Golden bytes for Tests/NetworkWireTest.cpp, built with Python's struct module straight
# from the protocol tables in docs/SteamMultiplayerImplementation.md, so they don't come
# from the C++ writer the test checks. Prints the kGolden table:
#
#     python3 Tests/fixtures/network_golden.py
import struct
def u8(v): return struct.pack('<B', v)
def i8(v): return struct.pack('<b', v)
def u16(v): return struct.pack('<H', v)
def i16(v): return struct.pack('<h', v)
def u32(v): return struct.pack('<I', v)
def u64(v): return struct.pack('<Q', v)
def f32(v): return struct.pack('<f', v)
def s(t): b=t.encode(); return u16(len(b))+b
def by(b): return u16(len(b))+bytes(b)
S = bytes(0x10+i for i in range(16))
Z = bytes(16)
C = bytes(0xC0+i for i in range(16))
DS = bytes((i*7+1)&0xFF for i in range(32))
DC = bytes((i*11+3)&0xFF for i in range(32))
DB = bytes((i*13+5)&0xFF for i in range(32))
STEAM1 = 76561197960265729
STEAM2 = 76561197960265730
def header(kind, session, tick, payload):
    return b'SWMP'+u16(1)+u8(kind)+u8(0)+session+u32(tick)+u32(len(payload))+payload
def pose():
    return (f32(1.5)+f32(-2.25)+f32(1000.0) + f32(0)+f32(0)+f32(0)+f32(1) +
            f32(0.5)+f32(0)+f32(-3) + f32(0)+f32(1.25)+f32(0) + u8(2))
def record(entity, inc, profile, state):
    return u64(entity)+u32(inc)+u16(profile)+u16(len(state))+state
def cmd(tt, inc, mx, mz, yaw, pitch, held, pressed, vt, vf):
    return u32(tt)+u32(inc)+i8(mx)+i8(mz)+u16(yaw)+i16(pitch)+u16(held)+u16(pressed)+u32(vt)+u16(vf)
assert len(pose())==53 and len(cmd(0,0,0,0,0,0,0,0,0,0))==24
msgs = [
 ("Hello", header(1, Z, 0, s("arena.v1")+DS+DC+u16(1)+u64(STEAM1)+C)),
 ("Challenge", header(2, S, 0, C+s("arena.v1")+DS+DC+u16(1)+u64(STEAM2)+u16(120)+u16(30)+u16(100)+u16(200))),
 ("AuthTicket", header(3, S, 0, by([0xDE,0xAD,0xBE,0xEF,0x01]))),
 ("Welcome", header(4, S, 0, u16(3)+u32(7)+u32(0xABCDEF)+u16(512))),
 ("BaselineBegin", header(5, S, 0, u32(7)+u32(0xABCDEF)+u16(2)+u32(138))),
 ("Spawn", header(6, S, 0, u32(1)+u64(0x100000002)+u32(1)+u16(3)+u16(1)+s("")+u16(32)+by([1,2,3]))),
 ("Despawn", header(7, S, 0, u32(2)+u64(42)+u8(1))),
 ("Ownership", header(8, S, 0, u32(3)+u64(42)+u32(2)+u16(4))),
 ("InitialState", header(9, S, 0, u32(7)+u16(1)+record(42,2,1,pose()))),
 ("BaselineEnd", header(10, S, 0, u32(7)+u32(3)+DB)),
 ("Ready", header(11, S, 0, u32(7)+DB)),
 ("Active", header(12, S, 1000, u32(1000)+u32(1004)+u64(42)+u32(2))),
 ("Inputs", header(13, S, 1004, u8(2)+cmd(1004,2,90,-90,0x8000,-1200,1,0,998,0x4000)+cmd(1005,2,0,127,0xFFFF,32767,3,3,999,0))),
 ("Snapshot", header(14, S, 1010, u32(55)+u32(1010)+u32(3)+u16(0)+u16(1)+u16(2)+record(42,2,1,pose())+record(STEAM1,0,33,bytes([10,11,12,13])))),
 ("Command", header(15, S, 1005, u32(9)+u64(0)+u16(32)+by([1]))),
 ("GameEvent", header(16, S, 1012, u32(4)+u32(1012)+u64(42)+u16(33)+by(range(8)))),
 ("Ping", header(17, S, 1013, u32(77)+u64(0x1234567890))),
 ("Pong", header(18, S, 1013, u32(77)+u64(0x1234567890)+u64(5000000)+u64(5000250)+u32(1013))),
 ("Reject", header(19, S, 0, u16(3)+s("server is full"))),
 ("Goodbye", header(20, S, 1014, u16(10))),
 ("ResyncRequest", header(21, S, 1009, u32(1009)+u8(2))),
 ("DeltaSnapshot", header(22, S, 1020, u32(1020)+u32(1014)+u32(3)+u8(1)+u8(0)+u8(2)+u16(5)+u16(12)+u16(2)+by([0xA5,0x3C,0x0F]))),
 ("SnapshotAck", header(23, S, 1021, u32(1020)+u32(0x80000001))),
]
print("// Golden bytes (see the comment at the top of this file).")
print("const GoldenMessage kGolden[] = {")
for name, b in msgs:
    h = b.hex()
    chunks = [h[i:i+96] for i in range(0, len(h), 96)]
    print(f'    {{"{name}",')
    for c in chunks: print(f'     "{c}"')
    print('    },')
print("};")
print("const char* kPoseHex = \"%s\";" % pose().hex())
