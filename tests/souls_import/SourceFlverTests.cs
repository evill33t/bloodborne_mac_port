// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Buffers.Binary;

internal static class SourceFlverTests {
    static void Check(bool ok,string why){if(!ok)throw new Exception(why);}
    internal static void Run(string? root) {
        var b=new byte[512+104];"FLVER\0L\0"u8.CopyTo(b);
        void I(int at,int value)=>BinaryPrimitives.WriteInt32LittleEndian(b.AsSpan(at,4),value);
        I(8,0x2000C);I(12,512);I(36,2);I(84,1);
        for(int n=0;n<2;n++){int at=128+n*32;I(at+8,n==0?36:32);I(at+12,n==0?2:1);I(at+24,n==0?72:32);I(at+28,n==0?0:72);}
        I(192,6);I(204,208);int[] types=[2,17,19,19,19,21],semantics=[0,2,3,6,10,5],offsets=[0,12,16,20,24,28];
        for(int n=0;n<6;n++){int at=208+n*20;I(at+4,offsets[n]);I(at+8,types[n]);I(at+12,semantics[n]);}
        for(int n=512;n<b.Length;n++)b[n]=(byte)n;
        var before=b.ToArray();var normalized=SourceFlver.Normalize(b,"DS1R");
        for(int v=0;v<2;v++) {
            Check(normalized.AsSpan(512+v*32,24).SequenceEqual(b.AsSpan(512+v*36,24)),"DSR repair changed position/bone/normal/tangent bytes");
            Check(normalized.AsSpan(512+v*32+24,8).SequenceEqual(b.AsSpan(512+v*36+28,8)),"DSR repair changed original color/UV bytes");
        }
        Check(normalized.AsSpan(584,32).SequenceEqual(b.AsSpan(584,32)),"Repair changed another shared-layout mesh");
        Check(before.SequenceEqual(b)&&SourceFlver.Normalize(b,"DS2S").SequenceEqual(b),"Repair modified source bytes or another game profile");
        if(!string.IsNullOrEmpty(root)) {
            string path=Path.Combine(root,"map/m10_02_00_00/m9000B2A10.flver.dcx");var original=File.ReadAllBytes(path);var raw=DCX.Decompress(original).ToArray();
            var model=SourceFlver.Read(original,"DS1R");Check(model.Meshes.Count==3&&model.Meshes.Sum(m=>m.Vertices.Count)==1090,"Source sky mesh counts changed");
            int data=BinaryPrimitives.ReadInt32LittleEndian(raw.AsSpan(12,4));int at=data+7472;
            // Independent source byte offsets from this installed model's original
            // 36-byte buffer header, rather than reserializing through the same reader.
            for(int v=0;v<352;v++) {
                var expected=new Vector3(BitConverter.ToSingle(raw,at+v*36),BitConverter.ToSingle(raw,at+v*36+4),BitConverter.ToSingle(raw,at+v*36+8));
                Check(model.Meshes[0].Vertices[v].Position==expected&&float.IsFinite(expected.LengthSquared()),"Recovered distant landscape position differs from source bytes");
                var color=model.Meshes[0].Vertices[v].Colors.Single();Check(color.R==raw[at+v*36+28]/255f&&color.A==raw[at+v*36+31]/255f,"Recovered source landscape color differs");
            }
            Check(File.ReadAllBytes(path).SequenceEqual(original),"Source sky asset changed");
        }
        Console.WriteLine("PASS: bounded DSR mixed-stride repair preserves original positions/colors/UVs and shared-layout meshes without source writes");
    }
}
