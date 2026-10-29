// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Buffers.Binary;
using System.Numerics;
using System.Text;

static class SourceCollisionTests {
    static void Check(bool value,string why){if(!value)throw new Exception(why);}
    internal static void Run() {
        var raw=Fixture();var surfaces=SourceCollision.Decode(raw);
        Check(surfaces.Count==1&&surfaces[0].Material==42&&surfaces[0].Triangles.Count==1,"Source physical material or triangle count changed");
        var triangle=surfaces[0].Triangles[0];
        Check(triangle.A==new Vector3(11,2,3)&&triangle.B==new Vector3(13,2,3)&&triangle.C==new Vector3(11,2,5),"Collision body and subpart transforms were not composed");
        var msb=new MSB1();msb.Models.Collisions.Add(new MSB1.Model.Collision{Name="h0000B2"});
        msb.Parts.Collisions.Add(new MSB1.Part.Collision{Name="floor",ModelName="h0000B2",HitFilterID=8,Position=new Vector3(2,0,0),Scale=Vector3.One});
        msb.Parts.Collisions.Add(new MSB1.Part.Collision{Name="camera",ModelName="h0000B2",HitFilterID=9,Position=new Vector3(20,0,0),Scale=Vector3.One});
        var files=new Dictionary<string,byte[]>{["map/MapStudio/m10_02_00_00.msb"]=msb.Write()};
        foreach(var layer in new[]{"h","l"}) {
            var binder=new BXF3();binder.Files.Add(new BinderFile(Binder.FileFlags.Flag1,0,$"{layer}0000B2A10.hkx.dcx",raw));
            binder.Write(out var header,out var body);files[$"map/m10_02_00_00/{layer}10_02_00_00.hkxbhd"]=header;files[$"map/m10_02_00_00/{layer}10_02_00_00.hkxbdt"]=body;
        }
        var result=SourceCollision.Read("DS1R","m10_02_00_00",files.GetValueOrDefault,new Vector3(100,0,0));
        Check(result.Parts.Count==4&&result.PlayerTriangles("h").Count==1&&result.PlayerTriangles("l").Count==1,"H/L layers or player/camera hit filters were conflated");
        var native=result.PlayerTriangles("h").Single();
        Check(native.A==new Vector3(113,2,3)&&native.B==new Vector3(113,2,5)&&native.C==new Vector3(115,2,3),"MSB instance/anchor transform or Bloodborne winding is wrong");
        Check(result.Parts.Single(p=>p.Layer=="h"&&p.HitFilter==9).Surfaces[0].Triangles[0].A==new Vector3(131,2,3),"Repeated source collision instance lost its own placement");
        var invalidIndex=raw.ToArray();BinaryPrimitives.WriteUInt16LittleEndian(invalidIndex.AsSpan(32+336,2),3);
        Reject(invalidIndex,"Out-of-range collision index accepted");
        var invalidReference=raw.ToArray();BinaryPrimitives.WriteUInt64LittleEndian(invalidReference.AsSpan(32,8),999);
        Reject(invalidReference,"Out-of-range collision reference accepted");
        var nan=raw.ToArray();BinaryPrimitives.WriteInt32LittleEndian(nan.AsSpan(32+288,4),0x7fc00000);
        Reject(nan,"Non-finite source collision coordinate accepted");
        Reject(raw[..^1],"Truncated collision file accepted");
        Console.WriteLine("PASS: original collision extraction preserves H/L layers, materials, body/subpart/instance transforms and filters; rejects malformed source data");
    }
    static void Reject(byte[] bytes,string why) {
        try{SourceCollision.Decode(bytes);}catch(InvalidDataException){return;}catch(NotSupportedException){return;}
        throw new Exception(why);
    }
    static byte[] Fixture() {
        // Minimal self-describing Havok 2015 collision fixture. Deliberately use
        // compact field layouts, so the reader must follow metadata, not retail
        // Firelink offsets or virtual object order.
        (string Name,int Size,(string Name,int At,int Type)[] Fields)[] types=[
            ("NULL",0,[]),
            ("hkpRigidBody",80,[("collidable",0,2),("motion",16,3)]),
            ("collidable",8,[("shape",0,16)]),
            ("motion",64,[("motionState",0,4)]),
            ("motionState",64,[("transform",0,5)]),
            ("hkTransformf",64,[]),
            ("CustomParamStorageExtendedMeshShape",64,[("meshstorage",0,7),("materialArray",16,7),("trianglesSubparts",32,7),("shapesSubparts",48,7)]),
            ("hkArray",16,[]),
            ("hkpStorageExtendedMeshShape::MeshSubpartStorage",64,[("vertices",0,7),("indices8",16,7),("indices16",32,7),("indices32",48,7)]),
            ("CustomMeshParameter",4,[("materialNameData",0,17)]),
            ("hkpExtendedMeshShape::TrianglesSubpart",64,[("numTriangleShapes",0,11),("indexStriding",4,12),("flipAlternateTriangles",6,13),("transform",16,14)]),
            ("hkInt32",4,[]),("hkUint16",2,[]),("hkUint8",1,[]),("hkQsTransformf",48,[]),("hkVector4",16,[]),("T*",8,[]),("hkUint32",4,[])
        ];
        static byte[] Bytes(Action<BinaryWriter> fill){using var stream=new MemoryStream();using(var w=new BinaryWriter(stream,Encoding.UTF8,true))fill(w);return stream.ToArray();}
        static byte[] Section(string name,byte[] body){var bytes=new byte[body.Length+8];BinaryPrimitives.WriteUInt32BigEndian(bytes,(uint)bytes.Length);Encoding.ASCII.GetBytes(name).CopyTo(bytes,4);body.CopyTo(bytes,8);return bytes;}
        static byte[] Join(params byte[][] values)=>values.SelectMany(b=>b).ToArray();
        static void Var(BinaryWriter w,int value){if(value<128)w.Write((byte)value);else if(value<16384){w.Write((byte)((value>>8)|128));w.Write((byte)value);}else throw new Exception("Fixture integer outside budget");}
        var strings=types.Skip(1).Select(t=>t.Name).Distinct().ToArray();var fields=types.SelectMany(t=>t.Fields).Select(f=>f.Name).Distinct().ToArray();
        var typeNames=Bytes(w=>{Var(w,types.Length);foreach(var type in types.Skip(1)){Var(w,Array.IndexOf(strings,type.Name));Var(w,0);}});
        var typeBodies=Bytes(w=>{for(int i=1;i<types.Length;++i){var type=types[i];Var(w,i);Var(w,0);Var(w,type.Fields.Length==0?8:40);Var(w,type.Size);Var(w,4);if(type.Fields.Length==0)continue;Var(w,type.Fields.Length);foreach(var field in type.Fields){Var(w,Array.IndexOf(fields,field.Name));Var(w,32);Var(w,field.At);Var(w,field.Type);}}});
        var data=new byte[360];
        void UInt64(int at,ulong value)=>BinaryPrimitives.WriteUInt64LittleEndian(data.AsSpan(at,8),value);
        void UInt32(int at,uint value)=>BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(at,4),value);
        void Float(int at,float value)=>BinaryPrimitives.WriteSingleLittleEndian(data.AsSpan(at,4),value);
        UInt64(0,2);for(int i=0;i<4;++i)Float(16+i*20,1);Float(16+48,10);
        UInt64(80,3);UInt64(96,4);UInt64(112,5);
        UInt64(144,8);UInt64(176,9);UInt32(208,42);
        UInt32(224,1);BinaryPrimitives.WriteUInt16LittleEndian(data.AsSpan(228,2),8);
        Float(240,1);Float(244,2);Float(248,3);Float(240+28,1);Float(240+32,2);Float(240+36,2);Float(240+40,2);
        Float(288+16,1);Float(288+32+8,1);
        foreach(var (index,value) in new[]{(0,0),(1,1),(2,2),(3,2)})BinaryPrimitives.WriteUInt16LittleEndian(data.AsSpan(336+index*2,2),(ushort)value);
        UInt64(344,6);UInt64(352,7);
        (int Type,int At,int Count)[] items=[(0,0,0),(1,0,1),(6,80,1),(16,344,1),(16,352,1),(10,224,1),(8,144,1),(9,208,1),(15,288,3),(12,336,4)];
        var indexBytes=Bytes(w=>{foreach(var item in items){w.Write((uint)item.Type|(item.Count==1?0x10000000u:0x20000000u));w.Write(item.At);w.Write(item.Count);}});
        return Section("TAG0",Join(Section("SDKV",Encoding.ASCII.GetBytes("20150100")),Section("DATA",data),Section("TYPE",Join(Section("TSTR",Encoding.ASCII.GetBytes(string.Join('\0',strings)+'\0')),Section("TNAM",typeNames),Section("FSTR",Encoding.ASCII.GetBytes(string.Join('\0',fields)+'\0')),Section("TBOD",typeBodies))),Section("INDX",Section("ITEM",indexBytes))));
    }
}
