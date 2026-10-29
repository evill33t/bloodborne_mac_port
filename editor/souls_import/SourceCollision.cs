// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Buffers.Binary;
using System.Numerics;
using System.Text;

// Reads the game's collision assets independently of its render meshes. DSR's
// Havok 2015 tagfiles carry their own type/member layouts; no SDK installation
// or guessed fixed offsets are needed to recover these static triangle meshes.
internal static class SourceCollision {
    internal sealed record Surface(uint Material,IReadOnlyList<MapCollision.Triangle> Triangles);
    internal sealed record Part(string Name,string Model,byte HitFilter,string Layer,IReadOnlyList<Surface> Surfaces,string Profile="DS1R") {
        internal bool PlayerSolid=>Profile is "DS2" or "DS2S"?HitFilter==100:HitFilter==8;
        internal int TriangleCount=>Surfaces.Sum(s=>s.Triangles.Count);
    }
    internal sealed record Result(IReadOnlyList<Part> Parts) {
        internal List<MapCollision.Triangle> PlayerTriangles(string layer)=>Parts.Where(p=>p.Layer==layer&&p.PlayerSolid).SelectMany(p=>p.Surfaces).SelectMany(s=>s.Triangles).ToList();
    }

    internal static Result Read(string profile,string map,Func<string,byte[]?> read,Vector3 offset) {
        if(profile!="DS1R")throw new NotSupportedException("Original collision extraction currently supports DS1 Remastered TAG0 assets");
        if(map.Length!=12||map[0]!='m'||!map.Skip(1).All(c=>char.IsAsciiDigit(c)||c=='_'))throw new ArgumentException("Invalid source map identifier");
        var msbData=read($"map/MapStudio/{map}.msb")??read($"map/MapStudio/{map}.msb.dcx")??throw new FileNotFoundException("Source collision MSB unavailable");
        var msb=MSB1.Read(msbData);var parts=new List<Part>();
        foreach(var layer in new[]{"h","l"}) {
            var stem=$"map/{map}/{layer}{map[1..]}.hkx";
            using var archive=new BXF3Reader(read(stem+"bhd")??throw new FileNotFoundException(stem+"bhd"),read(stem+"bdt")??throw new FileNotFoundException(stem+"bdt"));
            var files=archive.Files.ToDictionary(f=>Path.GetFileName(f.Name.Replace('\\','/')),StringComparer.OrdinalIgnoreCase);
            var decoded=new Dictionary<string,IReadOnlyList<Surface>>(StringComparer.Ordinal);
            foreach(var placement in msb.Parts.Collisions) {
                if(string.IsNullOrEmpty(placement.ModelName))throw new InvalidDataException("Source collision has no model");
                var model=layer+placement.ModelName[1..];
                if(!decoded.TryGetValue(model,out var surfaces)) {
                    var name=model+"A"+map.Substring(1,2)+".hkx.dcx";
                    if(!files.TryGetValue(name,out var file))throw new FileNotFoundException("Source collision model unavailable: "+name);
                    surfaces=Decode(archive.ReadFile(file).ToArray());decoded.Add(model,surfaces);
                }
                var rotation=placement.Rotation*(MathF.PI/180);
                var transform=Matrix4x4.CreateScale(placement.Scale)*Matrix4x4.CreateRotationX(rotation.X)*Matrix4x4.CreateRotationZ(rotation.Z)*Matrix4x4.CreateRotationY(rotation.Y)*Matrix4x4.CreateTranslation(placement.Position+offset);
                // DS1 collision winding is opposite Bloodborne's; mirror only the
                // index order, preserving the original world-space positions.
                var transformed=surfaces.Select(s=>new Surface(s.Material,s.Triangles.Select(t=>new MapCollision.Triangle(Vector3.Transform(t.A,transform),Vector3.Transform(t.C,transform),Vector3.Transform(t.B,transform))).ToArray())).ToArray();
                parts.Add(new Part(placement.Name,model,placement.HitFilterID,layer,transformed));
            }
        }
        return new Result(parts);
    }

    // Returns source-space winding. Read() applies the MSB instance transform and
    // Bloodborne winding once, including repeated collision model placements.
    internal static IReadOnlyList<Surface> Decode(byte[] input) {
        var raw=DCX.Is(input)?DCX.Decompress(input).ToArray():input;
        var tag=new Tag(raw);var surfaces=new List<Surface>();
        foreach(var body in tag.Items.Where(item=>tag.Name(item.Type)=="hkpRigidBody")) {
            var transform=tag.Transform(tag.Nested(body.Type,body.At,"motion","motionState","transform").At);
            var shape=tag.Reference(tag.Nested(body.Type,body.At,"collidable","shape").At);
            if(tag.Name(shape.Type)=="hkpMoppBvTreeShape")shape=tag.Reference(tag.Nested(shape.Type,shape.At,"child","childShape").At);
            if(tag.Name(shape.Type)!="CustomParamStorageExtendedMeshShape")throw new NotSupportedException("Unsupported source collision shape: "+tag.Name(shape.Type));
            var storage=tag.PointerArray(shape,"meshstorage");var materials=tag.PointerArray(shape,"materialArray");var subparts=tag.Array(shape,"trianglesSubparts");
            if(storage.Count!=materials.Count||storage.Count!=subparts.Count)throw new InvalidDataException("Collision storage, materials and subparts do not match");
            var shapeParts=tag.Array(shape,"shapesSubparts");
            if(shapeParts.Count!=0)throw new NotSupportedException("Convex child shapes require a separate collision importer");
            for(int i=0;i<storage.Count;++i) {
                var mesh=storage[i];var subpart=tag.Element(subparts,i);
                var vertices=tag.Array(mesh,"vertices");
                var arrays=new[]{("indices8",1),("indices16",2),("indices32",4)}.Select(pair=>(Item:tag.Array(mesh,pair.Item1),Stride:pair.Item2)).Where(pair=>pair.Item.Count>0).ToArray();
                if(arrays.Length!=1)throw new InvalidDataException("Expected one collision index array");
                var (indices,indexSize)=arrays[0];int count=tag.Int(tag.Nested(subpart.Type,subpart.At,"numTriangleShapes").At);
                int stride=tag.UShort(tag.Nested(subpart.Type,subpart.At,"indexStriding").At);
                if(count<0||count>1000000||stride!=indexSize*4||indices.Count!=count*4||vertices.Count<1||vertices.Count>1000000)throw new InvalidDataException("Invalid source collision triangle/index counts");
                if(tag.Byte(tag.Nested(subpart.Type,subpart.At,"flipAlternateTriangles").At)!=0)throw new NotSupportedException("Alternating collision strips are not supported");
                var local=tag.QsTransform(tag.Nested(subpart.Type,subpart.At,"transform").At)*transform;
                var points=new Vector3[vertices.Count];
                for(int v=0;v<points.Length;++v)points[v]=Vector3.Transform(tag.Vector(tag.Element(vertices,v).At),local);
                int Index(int n) {
                    int at=checked(indices.At+n*indexSize);uint value=indexSize==1?tag.Byte(at):indexSize==2?tag.UShort(at):tag.UInt(at);
                    return value<(uint)points.Length?(int)value:throw new InvalidDataException("Source collision index is outside its vertex array");
                }
                var triangles=new List<MapCollision.Triangle>(count);
                for(int t=0;t<count;++t) {
                    var a=points[Index(t*4)];var b=points[Index(t*4+1)];var c=points[Index(t*4+2)];
                    if(Vector3.Cross(b-a,c-a).LengthSquared()>1e-10f)triangles.Add(new(a,b,c));
                }
                uint material=tag.UInt(tag.Nested(materials[i].Type,materials[i].At,"materialNameData").At);
                surfaces.Add(new Surface(material,triangles));
            }
        }
        if(surfaces.Count==0)throw new InvalidDataException("No static source collision triangle meshes found");
        return surfaces;
    }

    internal sealed class Tag {
        internal sealed class TypeInfo {internal string Name="";internal int Parent,Size;internal readonly List<Field> Fields=[];}
        internal sealed record Field(string Name,int Offset,int Type);
        internal readonly record struct Item(int Type,int At,int Count);
        readonly byte[] data;readonly Dictionary<string,(int Start,int End)> sections=new();readonly List<TypeInfo> types=[];
        internal IReadOnlyList<Item> Items {get;}
        int cursor,limit;
        internal Tag(byte[] raw) {
            data=raw;if(data.Length<32||data.Length>128*1024*1024)throw new InvalidDataException("Source collision file outside read budget");
            Sections(0,data.Length,0);
            if(!sections.ContainsKey("TAG0")||Text("SDKV")!="20150100")throw new NotSupportedException("Expected Havok 2015 TAG0 collision");
            var names=Strings("TSTR");var fieldNames=Strings("FSTR");Set("TNAM");int count=Var();
            if(count<1||count>4096)throw new InvalidDataException("Invalid collision type count");
            for(int i=0;i<count;++i)types.Add(new TypeInfo());
            for(int i=1;i<count;++i) {
                types[i].Name=String(names,Var());int parameters=Var();if(parameters>32)throw new InvalidDataException("Invalid collision template parameter count");
                for(int p=0;p<parameters;++p){_ = String(names,Var());_ = Var();}
            }
            Set("TBOD");
            var defined=new HashSet<int>();
            for(int i=1;i<count;++i) {
                int index=Var();var type=Type(index);if(index==0||!defined.Add(index))throw new InvalidDataException("Duplicate collision type definition");
                type.Parent=Var();_ = Type(type.Parent);int optional=Var();if((optional&~127)!=0)throw new NotSupportedException("Unsupported collision type metadata");
                if((optional&1)!=0)_ = Var();if((optional&2)!=0)_ = Type(Var());if((optional&4)!=0)_ = Var();
                if((optional&8)!=0){type.Size=Var();_ = Var();if(type.Size>1024*1024)throw new InvalidDataException("Invalid collision type size");}
                if((optional&16)!=0)_ = Var();
                if((optional&32)!=0) {
                    int fields=Var()&0xffff;if(fields>256)throw new InvalidDataException("Invalid collision member count");
                    for(int f=0;f<fields;++f){var name=String(fieldNames,Var());_ = Var();int offset=Var(),member=Var();_ = Type(member);type.Fields.Add(new Field(name,offset,member));}
                }
                if((optional&64)!=0){int interfaces=Var();if(interfaces>64)throw new InvalidDataException("Invalid collision interface count");for(int j=0;j<interfaces;++j){_ = Type(Var());_ = Var();}}
            }
            var section=Section("ITEM");var start=Section("DATA").Start;
            if((section.End-section.Start)%12!=0||(section.End-section.Start)/12>50000)throw new InvalidDataException("Invalid collision item table");
            var items=new List<Item>();
            for(int at=section.Start;at<section.End;at+=12) {
                int type=(int)(UInt(at)&0xffffff);_ = Type(type);int offset=Int(at+4),length=Int(at+8);
                if(offset<0||length<0||length>4000000)throw new InvalidDataException("Invalid collision item range");
                int absolute=checked(start+offset);if(absolute<start||absolute>Section("DATA").End||(long)Size(type)*length>Section("DATA").End-absolute)throw new InvalidDataException("Collision item is outside DATA");
                items.Add(new Item(type,absolute,length));
            }
            Items=items;
        }
        void Sections(int start,int end,int depth) {
            if(depth>3)throw new InvalidDataException("Nested collision sections exceed budget");
            for(int at=start;at<end;) {
                Check(at,8);int size=(int)(BinaryPrimitives.ReadUInt32BigEndian(data.AsSpan(at,4))&0x3fffffff);
                if(size<8||size>end-at)throw new InvalidDataException("Invalid collision section size");
                string name=Encoding.ASCII.GetString(data,at+4,4);if(!sections.TryAdd(name,(at+8,at+size)))throw new InvalidDataException("Duplicate collision section");
                if(name is "TAG0" or "TYPE" or "INDX")Sections(at+8,at+size,depth+1);at+=size;
            }
        }
        (int Start,int End) Section(string name)=>sections.TryGetValue(name,out var section)?section:throw new InvalidDataException("Missing collision section "+name);
        string Text(string name){var section=Section(name);return Encoding.ASCII.GetString(data,section.Start,section.End-section.Start);}
        string[] Strings(string name)=>Text(name).Split('\0');
        static string String(string[] strings,int index)=>index>=0&&index<strings.Length?strings[index]:throw new InvalidDataException("Invalid collision string index");
        void Set(string name){var section=Section(name);cursor=section.Start;limit=section.End;}
        int Var() {
            if(cursor>=limit)throw new InvalidDataException("Truncated collision type metadata");byte first=data[cursor];
            foreach(var (bits,length,prefix) in new[]{(7,1,0),(14,2,128),(21,3,192),(27,4,224),(35,5,232),(40,6,248),(59,8,240),(64,9,249)}) {
                int mask=(~((1<<(bits%8))-1))&255;if((first&mask)!=prefix)continue;
                if(cursor+length>limit)throw new InvalidDataException("Truncated collision variable integer");
                ulong value=(ulong)(first&~mask);for(int i=1;i<length;++i)value=(value<<8)|data[cursor+i];cursor+=length;
                if(value>int.MaxValue)throw new InvalidDataException("Collision metadata integer exceeds budget");return(int)value;
            }
            throw new InvalidDataException("Invalid collision variable integer");
        }
        TypeInfo Type(int type)=>type>=0&&type<types.Count?types[type]:throw new InvalidDataException("Invalid collision type index");
        int Size(int type,int depth=0){if(depth>64)throw new InvalidDataException("Cyclic collision type hierarchy");var info=Type(type);return info.Size>0?info.Size:info.Parent>0?Size(info.Parent,depth+1):0;}
        internal string Name(int type)=>Type(type).Name;
        Field Find(int type,string name,int depth=0) {
            if(depth>64)throw new InvalidDataException("Cyclic collision member hierarchy");var info=Type(type);
            return info.Fields.FirstOrDefault(f=>f.Name==name)??(info.Parent>0?Find(info.Parent,name,depth+1):throw new InvalidDataException("Missing collision member "+info.Name+"."+name));
        }
        internal (int Type,int At) Nested(int type,int at,params string[] names) {
            foreach(var name in names){var field=Find(type,name);at=checked(at+field.Offset);type=field.Type;Check(at,Size(type));}return(type,at);
        }
        internal Item Reference(int at) {
            Check(at,8);ulong index=BinaryPrimitives.ReadUInt64LittleEndian(data.AsSpan(at,8));
            return index<(ulong)Items.Count?Items[(int)index]:throw new InvalidDataException("Collision reference is outside item table");
        }
        internal Item Array(Item item,string name)=>Reference(Nested(item.Type,item.At,name).At);
        internal List<Item> PointerArray(Item item,string name) {
            var array=Array(item,name);if(array.Count>4096)throw new InvalidDataException("Collision pointer array exceeds budget");
            var result=new List<Item>();for(int i=0;i<array.Count;++i)result.Add(Reference(checked(array.At+i*8)));return result;
        }
        internal Item Element(Item array,int index) {
            if(index<0||index>=array.Count)throw new InvalidDataException("Collision array index outside bounds");
            int stride=Size(array.Type);if(stride<1)throw new InvalidDataException("Missing collision array element size");
            int at=checked(array.At+index*stride);Check(at,stride);return new Item(array.Type,at,1);
        }
        void Check(int at,int count){if(at<0||count<0||at>data.Length-count)throw new InvalidDataException("Truncated collision data");}
        internal byte Byte(int at){Check(at,1);return data[at];}
        internal ushort UShort(int at){Check(at,2);return BinaryPrimitives.ReadUInt16LittleEndian(data.AsSpan(at,2));}
        internal uint UInt(int at){Check(at,4);return BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(at,4));}
        internal int Int(int at){Check(at,4);return BinaryPrimitives.ReadInt32LittleEndian(data.AsSpan(at,4));}
        float Float(int at){Check(at,4);float value=BitConverter.ToSingle(data,at);return float.IsFinite(value)&&Math.Abs(value)<=1000000?value:throw new InvalidDataException("Invalid collision coordinate/transform");}
        internal Vector3 Vector(int at)=>new(Float(at),Float(at+4),Float(at+8));
        internal Matrix4x4 Transform(int at)=>new(Float(at),Float(at+4),Float(at+8),Float(at+12),Float(at+16),Float(at+20),Float(at+24),Float(at+28),Float(at+32),Float(at+36),Float(at+40),Float(at+44),Float(at+48),Float(at+52),Float(at+56),Float(at+60));
        internal Matrix4x4 QsTransform(int at) {
            var translation=Vector(at);var rotation=new Quaternion(Float(at+16),Float(at+20),Float(at+24),Float(at+28));var scale=Vector(at+32);
            if(Math.Abs(rotation.LengthSquared()-1)>.001f)throw new InvalidDataException("Invalid collision subpart quaternion");
            return Matrix4x4.CreateScale(scale)*Matrix4x4.CreateFromQuaternion(rotation)*Matrix4x4.CreateTranslation(translation);
        }
    }
}
