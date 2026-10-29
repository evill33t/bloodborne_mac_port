// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Buffers.Binary;
using System.Numerics;
using System.Text;

static class LegacySourceCollisionTests {
    static void Check(bool value,string why){if(!value)throw new Exception(why);}
    internal static void Run() {
        var fixture=Fixture(out var dataAt);var original=fixture.ToArray();var surfaces=LegacySourceCollision.Decode(fixture);
        Check(surfaces.Count==1&&surfaces[0].Material==0x0013000b,"Scholar primitive material descriptor was lost or read as a DS1R name ID");
        var triangle=surfaces[0].Triangles.Single();
        Check(triangle.A==new Vector3(11,2,3)&&triangle.B==new Vector3(13,2,3)&&triangle.C==new Vector3(11,2,5),"Legacy body/subpart transforms or source winding changed");
        Check(fixture.SequenceEqual(original),"Legacy decoder modified its source");
        var msb=new MSB2();
        foreach(var name in new[]{"h01_0100","h99_0100"})msb.Models.Collisions.Add(new MSB2.Model.Collision{Name=name});
        msb.Parts.Collisions.Add(new MSB2.Part.Collision{Name="floor",ModelName="h01_0100",HitFilterID=MSB2.HitFilterType.Map,Scale=Vector3.One,Position=new Vector3(2,0,0)});
        msb.Parts.Collisions.Add(new MSB2.Part.Collision{Name="death",ModelName="h99_0100",HitFilterID=MSB2.HitFilterType.Fall_Death,Scale=Vector3.One});
        var files=new Dictionary<string,byte[]>{["map/m10_04_00_00/m10_04_00_00.msb"]=msb.Write()};
        foreach(var layer in new[]{"h","l"}) {
            var binder=new BXF4();binder.Files.Add(new BinderFile(Binder.FileFlags.Flag1,0,layer+"01_0100.hkx.dcx",fixture));
            if(layer=="h")binder.Files.Add(new BinderFile(Binder.FileFlags.Flag1,1,"h99_0100.hkx.dcx",fixture));
            binder.Write(out var header,out var body);files[$"model/map/{layer}10_04_00_00.hkxbhd"]=header;files[$"model/map/{layer}10_04_00_00.hkxbdt"]=body;
        }
        var result=LegacySourceCollision.Read("DS2S","m10_04_00_00",files.GetValueOrDefault,new Vector3(100,0,0));
        Check(result.Parts.Count==3&&result.PlayerTriangles("h").Count==1&&result.PlayerTriangles("l").Count==1,"Legacy solid100 / death1 filters or sparse H/L layers were conflated");
        Check(result.Parts.Single(p=>p.HitFilter==1).Profile=="DS2S"&&!result.Parts.Single(p=>p.HitFilter==1).PlayerSolid,"Legacy source filter metadata was overwritten");
        Check(result.PlayerTriangles("h").Single().A==new Vector3(113,2,3),"Legacy MSB instance and anchor transforms were lost");
        var invalidIndex=fixture.ToArray();BinaryPrimitives.WriteUInt16LittleEndian(invalidIndex.AsSpan(dataAt+1488,2),3);Reject(invalidIndex,"Invalid legacy triangle index accepted");
        var invalidEncoding=fixture.ToArray();BinaryPrimitives.WriteUInt32LittleEndian(invalidEncoding.AsSpan(dataAt+1232+16,4),0x9101);Reject(invalidEncoding,"Unknown legacy material encoding accepted");
        var nan=fixture.ToArray();BinaryPrimitives.WriteInt32LittleEndian(nan.AsSpan(dataAt+1440,4),0x7fc00000);Reject(nan,"Invalid legacy collision coordinate accepted");
        Console.WriteLine("PASS: Scholar legacy collision preserves source filters, packed surface descriptors, H/L layers, winding and transforms; rejects invalid geometry/encoding");
    }
    static void Reject(byte[] input,string why){try{LegacySourceCollision.Decode(input);}catch(InvalidDataException){return;}catch(NotSupportedException){return;}throw new Exception(why);}

    static byte[] Fixture(out int dataAt) {
        // Generated Havok packfile fixture, containing one triangle and no game
        // assets. Its local/global/virtual tables exercise the actual reader.
        var data=new byte[1536];
        void UInt(int at,uint value)=>BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(at,4),value);
        void Float(int at,float value)=>BinaryPrimitives.WriteSingleLittleEndian(data.AsSpan(at,4),value);
        for(int i=0;i<4;++i)Float(368+i*20,1);Float(368+48,10);
        UInt(720+240+8,1);UInt(720+320+8,1);UInt(720+352+8,1);
        UInt(1088+16+8,3);UInt(1088+48+8,4);
        UInt(1232+16,0x9100);UInt(1232+40+8,4);
        UInt(1296+32,1);BinaryPrimitives.WriteUInt16LittleEndian(data.AsSpan(1296+72,2),8);
        Float(1296+96,1);Float(1296+100,2);Float(1296+104,3);Float(1296+96+28,1);Float(1296+96+32,2);Float(1296+96+36,2);Float(1296+96+40,2);
        Float(1440+16,1);Float(1440+32+8,1);
        foreach(var (at,value) in new[]{(0,0),(1,1),(2,2),(3,2)})BinaryPrimitives.WriteUInt16LittleEndian(data.AsSpan(1488+at*2,2),(ushort)value);
        data[1513]=19;data[1515]=11;
        using var namesStream=new MemoryStream();using var namesWriter=new BinaryWriter(namesStream,Encoding.ASCII,true);
        var classes=new Dictionary<string,int>();
        foreach(var (name,signature) in new[]{("hkpRigidBody",0xcd2e69e5u),("CustomParamStorageExtendedMeshShape",0x1665eb05u),("hkpStorageExtendedMeshShapeMeshSubpartStorage",0xf41dfe52u),("CustomMeshParameter",0x81b795a4u)}) {
            namesWriter.Write(signature);namesWriter.Write((byte)9);classes[name]=(int)namesStream.Position;namesWriter.Write(Encoding.ASCII.GetBytes(name));namesWriter.Write((byte)0);
        }
        while(namesStream.Position%16!=0)namesWriter.Write((byte)255);var names=namesStream.ToArray();
        using var bodyStream=new MemoryStream();using var writer=new BinaryWriter(bodyStream,Encoding.UTF8,true);writer.Write(data);
        int local=(int)bodyStream.Position;
        foreach(var (from,to) in new[]{(960,1296),(1040,1496),(1072,1504),(1104,1440),(1136,1488),(1272,1512)}){writer.Write(from);writer.Write(to);}
        int global=(int)bodyStream.Position;
        foreach(var (from,to) in new[]{(32,720),(1496,1088),(1504,1232)}){writer.Write(from);writer.Write(2);writer.Write(to);}while(bodyStream.Position%16!=0)writer.Write(uint.MaxValue);
        int virtuals=(int)bodyStream.Position;
        foreach(var (at,name) in new[]{(0,"hkpRigidBody"),(720,"CustomParamStorageExtendedMeshShape"),(1088,"hkpStorageExtendedMeshShapeMeshSubpartStorage"),(1232,"CustomMeshParameter")}){writer.Write(at);writer.Write(0);writer.Write(classes[name]);}
        int end=(int)bodyStream.Position;
        using var stream=new MemoryStream();using var w=new BinaryWriter(stream,Encoding.UTF8,true);
        w.Write(0x57e0e057u);w.Write(0x10c0c010u);w.Write(0);w.Write(11);w.Write(new byte[]{8,1,0,1});w.Write(3);w.Write(2);w.Write(0);w.Write(0);w.Write(classes["hkpRigidBody"]);
        var version=new byte[16];Encoding.ASCII.GetBytes("hk_2014.1.0-r1").CopyTo(version,0);w.Write(version);w.Write(21);w.Write((short)0);w.Write((short)16);while(stream.Position<80)w.Write((byte)0);
        void Header(string tag,int start,int lo,int gl,int vi,int last) {
            var text=new byte[19];Encoding.ASCII.GetBytes(tag).CopyTo(text,0);w.Write(text);w.Write((byte)255);w.Write(start);w.Write(lo);w.Write(gl);w.Write(vi);w.Write(last);w.Write(last);w.Write(last);for(int i=0;i<4;++i)w.Write(uint.MaxValue);
        }
        int nameAt=272;dataAt=nameAt+names.Length;
        Header("__classnames__",nameAt,names.Length,names.Length,names.Length,names.Length);Header("__types__",dataAt,0,0,0,0);Header("__data__",dataAt,local,global,virtuals,end);
        w.Write(names);w.Write(bodyStream.ToArray());return stream.ToArray();
    }
}
