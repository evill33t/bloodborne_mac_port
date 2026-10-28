// SPDX-License-Identifier: GPL-2.0-or-later
// Havok 2014 PS4 collision: retain the native class layouts/materials and replace
// compressed mesh arrays and their local fixups. Codec layout follows Smithbox HKX2.
using SoulsFormats;
using System.Numerics;
using System.Text;

internal static class MapCollision {
    internal record Triangle(Vector3 A,Vector3 B,Vector3 C);
    sealed class Node {public Vector3 Min,Max;public Node? Left,Right;public int Index;public bool Leaf=>Left==null;}
    static Node Tree(List<Node> nodes){
        if(nodes.Count==1)return nodes[0];var lo=nodes.Select(n=>n.Min).Aggregate(Vector3.Min);var hi=nodes.Select(n=>n.Max).Aggregate(Vector3.Max);var span=hi-lo;int axis=span.X>span.Y?(span.X>span.Z?0:2):(span.Y>span.Z?1:2);
        float Mid(Node n)=>axis==0?n.Min.X+n.Max.X:axis==1?n.Min.Y+n.Max.Y:n.Min.Z+n.Max.Z;nodes.Sort((a,b)=>Mid(a).CompareTo(Mid(b)));int mid=nodes.Count/2;return new(){Min=lo,Max=hi,Left=Tree(nodes.GetRange(0,mid)),Right=Tree(nodes.GetRange(mid,nodes.Count-mid))};
    }
    static byte Compress(float min,float max,float pmin,float pmax){float scale=226/Math.Max(pmax-pmin,1e-8f);return (byte)((Math.Min(15,(int)MathF.Floor(MathF.Sqrt(Math.Max(0,(min-pmin)*scale))))<<4)|Math.Min(15,(int)MathF.Floor(MathF.Sqrt(Math.Max(0,(pmax-max)*scale)))));}
    static (float,float) Decompress(byte b,float lo,float hi){float unit=(hi-lo)/226;return (lo+(b>>4)*(b>>4)*unit,hi-(b&15)*(b&15)*unit);}
    static byte[] Codec(Node root,bool top,Dictionary<int,int>? leaves=null){
        var bytes=new List<byte>();int stride=top?5:4;
        void Add(Node n,Vector3 lo,Vector3 hi){int start=bytes.Count;var b=new[]{Compress(n.Min.X,n.Max.X,lo.X,hi.X),Compress(n.Min.Y,n.Max.Y,lo.Y,hi.Y),Compress(n.Min.Z,n.Max.Z,lo.Z,hi.Z)};bytes.AddRange(b);bytes.Add(0);if(top)bytes.Add(0);
            var x=Decompress(b[0],lo.X,hi.X);var y=Decompress(b[1],lo.Y,hi.Y);var z=Decompress(b[2],lo.Z,hi.Z);lo=new(x.Item1,y.Item1,z.Item1);hi=new(x.Item2,y.Item2,z.Item2);
            int value;if(n.Leaf){if(top)leaves?.Add(n.Index,start/stride);value=top?n.Index:n.Index*2;}else{Add(n.Left!,lo,hi);int offset=(bytes.Count-start)/stride;if(top){if(offset%2!=0||offset/2>32767)throw new InvalidDataException("Collision tree is too large");value=(offset/2)|0x8000;}else{if(offset>254||offset%2!=0)throw new InvalidDataException("Collision section tree is too large");value=offset|1;}Add(n.Right!,lo,hi);}bytes[start+3]=top?(byte)(value>>8):(byte)value;if(top)bytes[start+4]=(byte)value;
        }Add(root,root.Min,root.Max);return bytes.ToArray();
    }
    internal static byte[] Bake(byte[] input,IReadOnlyList<Triangle> triangles){
        if(triangles.Count<1||triangles.Count>1000000)throw new InvalidDataException("Collision triangle count outside import budget");
        foreach(var triangle in triangles)foreach(var p in new[]{triangle.A,triangle.B,triangle.C})if(!float.IsFinite(p.X)||!float.IsFinite(p.Y)||!float.IsFinite(p.Z)||Math.Abs(p.X)>1000000||Math.Abs(p.Y)>1000000||Math.Abs(p.Z)>1000000)throw new InvalidDataException("Invalid collision coordinates");
        var raw=DCX.Is(input)?DCX.Decompress(input).ToArray():input.ToArray();
        uint U(int at)=>BitConverter.ToUInt32(raw,at);
        if(raw.Length<256||U(0)!=0x57e0e057||raw[16]!=8||raw[17]!=1||raw[18]!=1)throw new InvalidDataException("Expected a PS4 Bloodborne collision template");
        int section=BitConverter.ToInt16(raw,62)+64;
        int names=(int)U(section+20),header=section+128,data=(int)U(header+20),local=(int)U(header+24),global=(int)U(header+28),virt=(int)U(header+32),end=(int)U(header+36);
        if(data<0||local<0||local>global||global>virt||virt>end||data+end>raw.Length)throw new InvalidDataException("Invalid collision fixup table");
        var fix=new Dictionary<int,int>();for(int o=data+local;o+8<=data+global;o+=8)if(U(o)!=uint.MaxValue)fix.Add((int)U(o),(int)U(o+4));
        var globals=new List<(int From,int Section,int To)>();for(int o=data+global;o+12<=data+virt;o+=12)if(U(o)!=uint.MaxValue)globals.Add(((int)U(o),(int)U(o+4),(int)U(o+8)));
        var virtuals=new List<(int At,int Section,int Name)>();var classes=new Dictionary<string,List<int>>();
        for(int o=data+virt;o+12<=data+end;o+=12)if(U(o)!=uint.MaxValue){int at=(int)U(o),sid=(int)U(o+4),name=(int)U(o+8);virtuals.Add((at,sid,name));var text=Encoding.ASCII.GetString(raw,names+name,System.Array.IndexOf(raw,(byte)0,names+name)-names-name);if(!classes.ContainsKey(text))classes[text]=[];classes[text].Add(at);}
        int One(string name)=>classes.TryGetValue(name,out var list)&&list.Count==1?list[0]:throw new InvalidDataException("Choose a collision template with one "+name);
        int shape=One("fsnpCustomParamCompressedMeshShape"),mesh=One("hknpCompressedMeshShapeData"),system=One("hknpPhysicsSystemData"),param=One("fsnpCustomMeshParameter"),tree=mesh+16;
        using var stream=new MemoryStream();stream.Write(raw,data,local);using var w=new BinaryWriter(stream,Encoding.UTF8,true);
        void At(int at,Action action){long pos=stream.Position;stream.Position=at;action();stream.Position=pos;}
        void Int(int at,uint value)=>At(at,()=>w.Write(value));
        void Pad(){while(stream.Position%16!=0)w.Write((byte)0);}
        int Append(byte[] bytes){Pad();int at=(int)stream.Position;w.Write(bytes);return at;}
        void Array(int at,int destination,int count){At(at,()=>{w.Write(0UL);w.Write(count);w.Write((uint)count|0x80000000u);});fix.Remove(at);if(count>0)fix.Add(at,destination);}
        byte[] Make(Action<BinaryWriter> action){using var s=new MemoryStream();using(var writer=new BinaryWriter(s,Encoding.UTF8,true))action(writer);return s.ToArray();}
        void Vec(BinaryWriter b,Vector3 v){b.Write(v.X);b.Write(v.Y);b.Write(v.Z);b.Write(1f);}
        var groups=new List<List<Triangle>>();
        void Split(List<Triangle> list){if(list.Count<=63){groups.Add(list);return;}var nodes=list.Select((t,i)=>new Node{Min=Vector3.Min(t.A,Vector3.Min(t.B,t.C)),Max=Vector3.Max(t.A,Vector3.Max(t.B,t.C)),Index=i}).ToList();var lo=nodes.Select(n=>n.Min).Aggregate(Vector3.Min);var hi=nodes.Select(n=>n.Max).Aggregate(Vector3.Max);var d=hi-lo;int a=d.X>d.Y?(d.X>d.Z?0:2):(d.Y>d.Z?1:2);float Key(Triangle t)=>a==0?t.A.X+t.B.X+t.C.X:a==1?t.A.Y+t.B.Y+t.C.Y:t.A.Z+t.B.Z+t.C.Z;list.Sort((x,y)=>Key(x).CompareTo(Key(y)));int mid=list.Count/2;Split(list.GetRange(0,mid));Split(list.GetRange(mid,list.Count-mid));}
        Split(triangles.ToList());var sectionNodeOffsets=new List<int>();var sectionBytes=new List<byte[]>();var sections=new List<Node>();var packed=new List<uint>();var primitives=new List<byte>();var keys=new List<uint>();
        for(int s=0;s<groups.Count;++s){var group=groups[s];var all=group.SelectMany(t=>new[]{t.A,t.B,t.C}).ToArray();var lo=all.Aggregate(Vector3.Min)-new Vector3(.001f);var hi=all.Aggregate(Vector3.Max)+new Vector3(.001f);var scale=(hi-lo)/new Vector3(2047,2047,1023);int first=packed.Count,prim=primitives.Count/4;
            uint Q(Vector3 p){p=(p-lo)/scale;return (uint)Math.Clamp((int)MathF.Round(p.X),0,2047)|((uint)Math.Clamp((int)MathF.Round(p.Y),0,2047)<<11)|((uint)Math.Clamp((int)MathF.Round(p.Z),0,1023)<<22);}
            foreach(var p in all)packed.Add(Q(p));var leaves=new List<Node>();for(int i=0;i<group.Count;++i){var t=group[i];leaves.Add(new(){Min=Vector3.Min(t.A,Vector3.Min(t.B,t.C))-new Vector3(.001f),Max=Vector3.Max(t.A,Vector3.Max(t.B,t.C))+new Vector3(.001f),Index=i});primitives.AddRange(new[]{(byte)(i*3),(byte)(i*3+1),(byte)(i*3+2),(byte)(i*3+2)});keys.Add((uint)((s<<8)|i));}
            var sectionTree=Tree(leaves);sectionTree.Min=lo;sectionTree.Max=hi;var codec=Codec(sectionTree,false);int nodeAt=Append(codec);
            sectionBytes.Add(Make(b=>{b.Write(0UL);b.Write(codec.Length/4);b.Write((uint)(codec.Length/4)|0x80000000u);Vec(b,lo);Vec(b,hi);b.Write(lo.X);b.Write(lo.Y);b.Write(lo.Z);b.Write(scale.X);b.Write(scale.Y);b.Write(scale.Z);b.Write(first);b.Write((uint)all.Length);b.Write((uint)((prim<<8)|group.Count));b.Write((uint)((s<<8)|1));b.Write((byte)all.Length);b.Write((byte)0);b.Write((ushort)0);b.Write(0u);}));
            sections.Add(new(){Min=lo,Max=hi,Index=s});sectionNodeOffsets.Add(nodeAt);
        }
        var top=Tree(sections);var leafNodes=new Dictionary<int,int>();var topBytes=Codec(top,true,leafNodes);foreach(var (index,node) in leafNodes)BitConverter.GetBytes(checked((ushort)node)).CopyTo(sectionBytes[index],90);int topAt=Append(topBytes);Array(tree,topAt,topBytes.Length/5);At(tree+16,()=>{Vec(w,top.Min);Vec(w,top.Max);});
        int bits=32-System.Numerics.BitOperations.LeadingZeroCount(keys.Max()+1);Int(tree+48,(uint)triangles.Count);Int(tree+52,(uint)bits);Int(tree+56,keys.Max());
        int sectionAt=Append(sectionBytes.SelectMany(b=>b).ToArray());Array(tree+64,sectionAt,groups.Count);for(int s=0;s<groups.Count;++s)fix.Add(sectionAt+s*96,sectionNodeOffsets[s]);
        Array(tree+80,Append(primitives.ToArray()),primitives.Count/4);Array(tree+96,0,0);Array(tree+112,Append(Make(b=>{foreach(var p in packed)b.Write(p);})),packed.Count);Array(tree+128,0,0);
        var runs=Make(b=>{foreach(var group in groups){b.Write(ushort.MaxValue);b.Write((byte)0);b.Write((byte)group.Count);}});Array(tree+144,Append(runs),groups.Count);
        // Keep the native disabled SIMD sentinel nodes; queries use the complete binary BVH.
        // Clear old welding and bit masks indexed by the previous mesh's shape keys.
        for(int o=shape+40;o<shape+88;o+=4)Int(o,0);Int(shape+40,uint.MaxValue);Int(shape+80,uint.MaxValue);foreach(var key in fix.Keys.Where(k=>k>=shape+40&&k<shape+88).ToArray())fix.Remove(key);
        Array(shape+96,0,0);Int(shape+112,0);Array(shape+120,0,0);Int(shape+136,0);At(shape+18,()=>w.Write((byte)bits));
        Array(shape+160,Append(Make(b=>{foreach(var key in keys)b.Write((key<<(32-bits))|((1u<<(32-bits))-1));})),keys.Count);
        // All generated triangles share the template's first physical surface material.
        var surface=raw.AsSpan(data+fix[param+16],20).ToArray();var surfaces=new byte[(triangles.Count+1)*20];for(int i=0;i<=triangles.Count;++i)surface.CopyTo(surfaces,i*20);Array(param+16,Append(surfaces),triangles.Count+1);
        var body=fix[system+64];int bodies=(int)U(data+system+72);if(bodies!=1)throw new InvalidDataException("Expected one collision body");At(body+48,()=>{Vec(w,Vector3.Zero);w.Write(0f);w.Write(0f);w.Write(0f);w.Write(1f);});
        Pad();int newLocal=(int)stream.Position;foreach(var (from,to) in fix.OrderBy(p=>p.Key)){w.Write(from);w.Write(to);}while(stream.Position%16!=0)w.Write(uint.MaxValue);
        int newGlobal=(int)stream.Position;foreach(var (from,sid,to) in globals){w.Write(from);w.Write(sid);w.Write(to);}while(stream.Position%16!=0)w.Write(uint.MaxValue);
        int newVirtual=(int)stream.Position;foreach(var (at,sid,name) in virtuals){w.Write(at);w.Write(sid);w.Write(name);}while(stream.Position%16!=0)w.Write(uint.MaxValue);int newEnd=(int)stream.Position;
        var prefix=raw.AsSpan(0,data).ToArray();void Header(int o,int value)=>BitConverter.GetBytes(value).CopyTo(prefix,o);Header(header+24,newLocal);Header(header+28,newGlobal);Header(header+32,newVirtual);for(int o=36;o<=44;o+=4)Header(header+o,newEnd);
        return prefix.Concat(stream.ToArray()).ToArray();
    }
    internal static List<List<Triangle>> Partition(List<Triangle> triangles,int count){
        if(count<1||count>1000||triangles.Count<count)throw new InvalidDataException("Invalid collision partition count");var groups=new List<List<Triangle>>();
        void Split(List<Triangle> list,int n){if(n==1){groups.Add(list);return;}var lo=list.Select(t=>(t.A+t.B+t.C)/3).Aggregate(Vector3.Min);var hi=list.Select(t=>(t.A+t.B+t.C)/3).Aggregate(Vector3.Max);var d=hi-lo;int axis=d.X>d.Y?(d.X>d.Z?0:2):(d.Y>d.Z?1:2);float Key(Triangle t)=>axis==0?t.A.X+t.B.X+t.C.X:axis==1?t.A.Y+t.B.Y+t.C.Y:t.A.Z+t.B.Z+t.C.Z;list.Sort((a,b)=>Key(a).CompareTo(Key(b)));int left=n/2,mid=list.Count*left/n;Split(list.GetRange(0,mid),left);Split(list.GetRange(mid,list.Count-mid),n-left);}
        Split(triangles.ToList(),count);return groups;
    }
    internal static byte[] RenameBody(byte[] input,string from,string to){
        if(from.Length!=7||to.Length!=7||from[0]!=to[0]||from[0] is not('h' or 'l')||from[1..].Any(c=>c<'0'||c>'9')||to[1..].Any(c=>c<'0'||c>'9'))throw new InvalidDataException("Invalid native collision body name");
        var result=input.ToArray();var old=Encoding.ASCII.GetBytes(from);var replacement=Encoding.ASCII.GetBytes(to);int count=0;for(int i=0;i+7<result.Length;++i)if(result.AsSpan(i,7).SequenceEqual(old)&&result[i+7]==0){replacement.CopyTo(result,i);++count;}if(count!=1)throw new InvalidDataException("Expected one native collision body name");return result;
    }
    // Collision part transforms are not sufficient for relocating every retail HKX
    // body. Move the bodies themselves, keeping all native names and references.
    internal static byte[] Translate(byte[] input,Vector3 offset){
        var raw=DCX.Is(input)?DCX.Decompress(input).ToArray():input.ToArray();
        uint U(int at)=>BitConverter.ToUInt32(raw,at);
        if(raw.Length<256||U(0)!=0x57e0e057||raw[16]!=8||raw[17]!=1||raw[18]!=1)throw new InvalidDataException("Expected native PS4 collision");
        int section=BitConverter.ToInt16(raw,62)+64,names=(int)U(section+20),header=section+128,data=(int)U(header+20),local=(int)U(header+24),global=(int)U(header+28),virt=(int)U(header+32),end=(int)U(header+36);
        var fix=new Dictionary<int,int>();for(int o=data+local;o+8<=data+global;o+=8)if(U(o)!=uint.MaxValue)fix.Add((int)U(o),(int)U(o+4));
        for(int o=data+virt;o+12<=data+end;o+=12){if(U(o)==uint.MaxValue)continue;int name=names+(int)U(o+8);var cls=Encoding.ASCII.GetString(raw,name,System.Array.IndexOf(raw,(byte)0,name)-name);if(cls!="hknpPhysicsSystemData")continue;int system=(int)U(o),count=(int)U(data+system+72);if(count<0||count>10000||!fix.TryGetValue(system+64,out var body))throw new InvalidDataException("Invalid native collision body array");for(int i=0;i<count;++i)for(int c=0;c<3;++c){int at=data+body+i*96+48+c*4;float value=BitConverter.ToSingle(raw,at)+(c==0?offset.X:c==1?offset.Y:offset.Z);BitConverter.GetBytes(value).CopyTo(raw,at);}}
        return raw;
    }
}
