// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Buffers.Binary;
using System.Numerics;

internal static class LegacySourceCollision {
    internal static SourceCollision.Result Read(string profile,string map,Func<string,byte[]?> read,Vector3 offset) {
        if(profile is not("DS2" or "DS2S"))throw new NotSupportedException("Legacy collision adapter expects DS2/Scholar assets");
        var msb=MSB2.Read(read($"map/{map}/{map}.msb")??throw new FileNotFoundException("Source collision MSB unavailable"));
        var parts=new List<SourceCollision.Part>();
        foreach(var layer in new[]{"h","l"}) {
            var stem=$"model/map/{layer}{map[1..]}.hkx";
            using var archive=new BXF4Reader(read(stem+"bhd")??throw new FileNotFoundException(stem+"bhd"),read(stem+"bdt")??throw new FileNotFoundException(stem+"bdt"));
            var files=archive.Files.ToDictionary(f=>Path.GetFileName(f.Name.Replace('\\','/')),StringComparer.OrdinalIgnoreCase);
            foreach(var placement in msb.Parts.Collisions) {
                var model=layer+placement.ModelName[1..];var name=model+".hkx.dcx";
                if(!files.TryGetValue(name,out var file)) {
                    // DS2's death, camera and enemy-boundary volumes commonly
                    // have H records only. A missing walkable layer is an error.
                    if(layer=="l"&&placement.HitFilterID!=MSB2.HitFilterType.Map)continue;
                    throw new FileNotFoundException("Source collision model unavailable: "+name);
                }
                var rotation=placement.Rotation*(MathF.PI/180);
                var transform=Matrix4x4.CreateScale(placement.Scale)*Matrix4x4.CreateRotationX(rotation.X)*Matrix4x4.CreateRotationZ(rotation.Z)*Matrix4x4.CreateRotationY(rotation.Y)*Matrix4x4.CreateTranslation(placement.Position+offset);
                var surfaces=Decode(archive.ReadFile(file).ToArray()).Select(s=>new SourceCollision.Surface(s.Material,s.Triangles.Select(t=>new MapCollision.Triangle(Vector3.Transform(t.A,transform),Vector3.Transform(t.B,transform),Vector3.Transform(t.C,transform))).ToArray())).ToArray();
                // DS2's source collision winding already matches Bloodborne.
                parts.Add(new(placement.Name,model,(byte)placement.HitFilterID,layer,surfaces,profile));
            }
        }
        return new SourceCollision.Result(parts);
    }

    internal static IReadOnlyList<SourceCollision.Surface> Decode(byte[] input) {
        var raw=DCX.Is(input)?DCX.Decompress(input).ToArray():input;
        var pack=new Pack(raw);var result=new List<SourceCollision.Surface>();
        foreach(var body in pack.Classes.Where(p=>p.Value=="hkpRigidBody").Select(p=>p.Key)) {
            var world=pack.Transform(body+336+32);var shape=pack.Global(body+32);
            if(pack.Classes[shape]=="hkpMoppBvTreeShape")shape=pack.Global(shape+88);
            pack.Expect(shape,"CustomParamStorageExtendedMeshShape");
            var meshes=pack.Pointers(shape+320);var parameters=pack.Pointers(shape+352);var subparts=pack.Array(shape+240,144);
            if(meshes.Count!=parameters.Count||meshes.Count!=subparts.Count||pack.Array(shape+256,80).Count!=0)throw new InvalidDataException("Unsupported legacy collision subpart layout");
            for(int i=0;i<meshes.Count;++i) {
                int mesh=meshes[i],parameter=parameters[i],subpart=subparts.At+i*144;
                pack.Expect(mesh,"hkpStorageExtendedMeshShapeMeshSubpartStorage");pack.Expect(parameter,"CustomMeshParameter");
                var vertices=pack.Array(mesh+16,16);
                var indices=new[]{(Offset:32,Width:1),(Offset:48,Width:2),(Offset:64,Width:4)}.Select(p=>(Array:pack.Array(mesh+p.Offset,p.Width),p.Width)).Where(p=>p.Array.Count!=0).ToArray();
                int count=pack.Int(subpart+32);
                if(indices.Length!=1||count<0||count>1000000||vertices.Count<1||vertices.Count>1000000||indices[0].Array.Count!=count*4||pack.Short(subpart+72)!=indices[0].Width*4||pack.Byte(subpart+75)!=0)throw new InvalidDataException("Invalid legacy collision triangle layout");
                var vertexData=pack.Array(parameter+24,1);var primitiveData=pack.Array(parameter+40,1);
                if(pack.UInt(parameter+16)!=0x9100||vertexData.Count!=0||pack.Int(parameter+36)!=0||primitiveData.Count!=4||pack.UInt(parameter+52)!=0)throw new NotSupportedException("Unsupported legacy CustomMeshParameter encoding");
                // In Scholar this four-byte primitive descriptor carries the
                // surface data, while materialNameData is zero. Preserve all
                // bytes as a packed value; do not treat it as DS1R's material ID.
                uint material=pack.BigUInt(primitiveData.At);
                var local=pack.QsTransform(subpart+96)*world;var points=new Vector3[vertices.Count];
                for(int v=0;v<points.Length;++v)points[v]=Vector3.Transform(pack.Vector(vertices.At+v*16),local);
                var (index,width)=indices[0];
                int Index(int n){int at=index.At+n*width;uint value=width==1?pack.Byte(at):width==2?pack.Short(at):pack.UInt(at);return value<(uint)points.Length?(int)value:throw new InvalidDataException("Legacy collision index is outside its vertices");}
                var triangles=new List<MapCollision.Triangle>(count);
                for(int t=0;t<count;++t){var a=points[Index(t*4)];var b=points[Index(t*4+1)];var c=points[Index(t*4+2)];if(Vector3.Cross(b-a,c-a).LengthSquared()>1e-10f)triangles.Add(new(a,b,c));}
                result.Add(new(material,triangles));
            }
        }
        if(result.Count==0)throw new InvalidDataException("No legacy static collision triangles found");return result;
    }

    sealed class Pack {
        readonly byte[] data;readonly Dictionary<int,int> local,global;
        internal Dictionary<int,string> Classes {get;}
        internal Pack(byte[] raw) {
            if(raw.Length<256||raw.Length>128*1024*1024||raw[16]!=8||raw[17]!=1)throw new NotSupportedException("Expected 64-bit little-endian Scholar collision");
            // Smithbox reads the packfile tables and retains unknown class data.
            // Offsets below follow its checked HKX2 classxmlds3 layouts; validate
            // the corresponding class signatures before using those layouts.
            var hkx=HKX.Read(raw,HKX.HKXVariation.HKXDS3,false);
            if(hkx.Header.Version!=11||hkx.Header.ContentsVersionString!="hk_2014.1.0-r1")throw new NotSupportedException("Expected Havok 2014.1 collision packfile");
            data=hkx.DataSection.SectionData;
            local=hkx.DataSection.LocalFixups.ToDictionary(f=>checked((int)f.Src),f=>checked((int)f.Dst));
            global=new();foreach(var fixup in hkx.DataSection.GlobalFixups){if(fixup.DstSectionIndex!=2)throw new NotSupportedException("Cross-section collision references are not supported");global.Add(checked((int)fixup.Src),checked((int)fixup.Dst));}
            var signatures=new Dictionary<string,uint>{{"hkpRigidBody",0xcd2e69e5},{"hkpMoppBvTreeShape",0x73f28bee},{"CustomParamStorageExtendedMeshShape",0x1665eb05},{"hkpStorageExtendedMeshShapeMeshSubpartStorage",0xf41dfe52},{"CustomMeshParameter",0x81b795a4}};
            Classes=new();foreach(var item in hkx.DataSection.VirtualReferences){var name=item.ClassName.ClassName;if(signatures.TryGetValue(name,out var signature)&&signature!=item.ClassName.Signature)throw new NotSupportedException("Unsupported collision class signature: "+name);Classes.Add(checked((int)item.SourceObject.SectionOffset),name);}
        }
        internal void Expect(int at,string name){if(!Classes.TryGetValue(at,out var actual)||actual!=name)throw new InvalidDataException("Expected legacy collision class "+name);}
        void Check(int at,int count){if(at<0||count<0||at>data.Length-count)throw new InvalidDataException("Legacy collision range outside section");}
        internal byte Byte(int at){Check(at,1);return data[at];}
        internal ushort Short(int at){Check(at,2);return BinaryPrimitives.ReadUInt16LittleEndian(data.AsSpan(at,2));}
        internal uint UInt(int at){Check(at,4);return BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(at,4));}
        internal int Int(int at){Check(at,4);return BinaryPrimitives.ReadInt32LittleEndian(data.AsSpan(at,4));}
        internal uint BigUInt(int at){Check(at,4);return BinaryPrimitives.ReadUInt32BigEndian(data.AsSpan(at,4));}
        internal int Global(int at)=>global.TryGetValue(at,out var destination)&&Classes.ContainsKey(destination)?destination:throw new InvalidDataException("Missing legacy collision object reference");
        internal (int At,int Count) Array(int at,int stride) {
            int count=Int(at+8);if(count<0||count>4000000)throw new InvalidDataException("Legacy collision array exceeds budget");
            if(count==0)return(0,0);if(!local.TryGetValue(at,out var destination))throw new InvalidDataException("Missing legacy collision array fixup");
            Check(destination,checked(count*stride));return(destination,count);
        }
        internal List<int> Pointers(int at){var array=Array(at,8);if(array.Count>4096)throw new InvalidDataException("Legacy collision pointer array exceeds budget");return Enumerable.Range(0,array.Count).Select(i=>Global(array.At+i*8)).ToList();}
        float Float(int at){Check(at,4);float value=BitConverter.ToSingle(data,at);return float.IsFinite(value)&&Math.Abs(value)<=1000000?value:throw new InvalidDataException("Invalid legacy collision coordinate");}
        internal Vector3 Vector(int at)=>new(Float(at),Float(at+4),Float(at+8));
        internal Matrix4x4 Transform(int at)=>new(Float(at),Float(at+4),Float(at+8),Float(at+12),Float(at+16),Float(at+20),Float(at+24),Float(at+28),Float(at+32),Float(at+36),Float(at+40),Float(at+44),Float(at+48),Float(at+52),Float(at+56),Float(at+60));
        internal Matrix4x4 QsTransform(int at){var translation=Vector(at);var rotation=new Quaternion(Float(at+16),Float(at+20),Float(at+24),Float(at+28));var scale=Vector(at+32);if(Math.Abs(rotation.LengthSquared()-1)>.001f)throw new InvalidDataException("Invalid legacy collision quaternion");return Matrix4x4.CreateScale(scale)*Matrix4x4.CreateFromQuaternion(rotation)*Matrix4x4.CreateTranslation(translation);}
    }
}
