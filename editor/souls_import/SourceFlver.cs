// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Buffers.Binary;

// DSR occasionally stores an extra four-byte tangent immediately after its
// declared tangent. SoulsFormats changes a shared layout while reading this,
// corrupting a 36-byte mesh when the same layout is also used by a 32-byte mesh.
// Compact only this verified encoding in a private copy before calling the reader.
internal static class SourceFlver {
    internal static FLVER2 Read(Memory<byte> input,string profile) {
        var raw=DCX.Is(input.ToArray())?DCX.Decompress(input).ToArray():input.ToArray();
        return FLVER2.Read(Normalize(raw,profile));
    }
    internal static byte[] Normalize(byte[] input,string profile) {
        var b=input.ToArray();
        if(profile!="DS1R"||b.Length<128||!b.AsSpan(0,8).SequenceEqual("FLVER\0L\0"u8))return b;
        int I(int at){if(at<0||at>b.Length-4)throw new InvalidDataException("Truncated source FLVER metadata");return BinaryPrimitives.ReadInt32LittleEndian(b.AsSpan(at,4));}
        if(I(8)!=0x2000C)return b;
        int Count(int at){int n=I(at);if(n<0||n>1000000)throw new InvalidDataException("Invalid source FLVER count");return n;}
        int data=I(12),dummies=Count(20),materials=Count(24),nodes=Count(28),meshes=Count(32),buffers=Count(36),faces=Count(80),layouts=Count(84);
        long bufferAt=128L+dummies*64L+materials*32L+nodes*128L+meshes*48L+faces*32L;
        long layoutAt=bufferAt+buffers*32L;
        if(data<128||data>b.Length||bufferAt>data||layoutAt+layouts*16L>data)throw new InvalidDataException("Source FLVER tables exceed metadata");
        for(int n=0;n<buffers;n++) {
            int at=checked((int)bufferAt+n*32),layout=I(at+4),stride=I(at+8),count=Count(at+12),offset=I(at+28);
            if(layout<0||layout>=layouts)throw new InvalidDataException("Source FLVER layout reference out of range");
            int header=checked((int)layoutAt+layout*16),members=Count(header),memberAt=I(header+12);
            if(stride!=36||members!=6)continue;
            int[] types=[2,17,19,19,19,21],semantics=[0,2,3,6,10,5],offsets=[0,12,16,20,24,28];
            bool known=true;
            for(int k=0;k<6;k++) {
                int m=checked(memberAt+k*20);
                if(I(m)!=0||I(m+4)!=offsets[k]||I(m+8)!=types[k]||I(m+12)!=semantics[k]||I(m+16)!=0)known=false;
            }
            if(!known)continue;
            long begin=(long)data+offset,end=begin+count*36L;
            if(offset<0||begin<data||end>b.Length||I(at+24)!=count*36L)throw new InvalidDataException("Source FLVER vertex buffer exceeds data");
            var source=b.AsSpan((int)begin,count*36).ToArray();
            for(int v=0;v<count;v++) {
                source.AsSpan(v*36,24).CopyTo(b.AsSpan((int)begin+v*32,24));
                source.AsSpan(v*36+28,8).CopyTo(b.AsSpan((int)begin+v*32+24,8));
            }
            BinaryPrimitives.WriteInt32LittleEndian(b.AsSpan(at+8,4),32);
            BinaryPrimitives.WriteInt32LittleEndian(b.AsSpan(at+24,4),checked(count*32));
        }
        return b;
    }
}
