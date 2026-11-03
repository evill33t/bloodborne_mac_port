// SPDX-License-Identifier: GPL-3.0-or-later
using System.Numerics;
using System.Security.Cryptography;

// The rotating bridge is an authored convex solid, including its spikes.
// Replace the native keyframed leaf's one shape; retain its scene/controller ABI.
internal static class CatacombsBridgePhysics {
    internal sealed record Hull(Vector3[] Vertices,Vector4[] Planes,byte[][] Faces,float Radius);
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Catacombs bridge collision: "+why);}
    internal static Hull Source(byte[] raw) {
        Need(System.Convert.ToHexStringLower(SHA256.HashData(raw))=="558de473c52b9e8400c106c35b94e160edf6f544393114c3b309780721c3b80a","source o3001 physics changed");
        var t=new SourceCollision.Tag(raw);var body=t.Items.Single(i=>i.Count==1&&t.Name(i.Type)=="hkpRigidBody");
        var shape=t.Reference(t.Nested(body.Type,body.At,"collidable","shape").At);Need(t.Name(shape.Type)=="hkpConvexVerticesShape","source bridge hull type");
        var vertices=t.Array(shape,"rotatedVertices");var planes=t.Array(shape,"planeEquations");var connectivity=t.Reference(t.Nested(shape.Type,shape.At,"connectivity").At);
        var counts=t.Array(connectivity,"numVerticesPerFace");var indices=t.Array(connectivity,"vertexIndices");
        Need(t.Int(t.Nested(shape.Type,shape.At,"numVertices").At)==24&&vertices.Count==6&&t.ByteSize(vertices.Type)==48&&planes.Count==24&&counts.Count==24&&indices.Count==88,"source bridge topology");
        float F(int at)=>BitConverter.ToSingle(raw,at);
        var points=Enumerable.Range(0,24).Select(i=>new Vector3(F(vertices.At+i/4*48+i%4*4),F(vertices.At+i/4*48+16+i%4*4),F(vertices.At+i/4*48+32+i%4*4))).ToArray();
        var equations=Enumerable.Range(0,24).Select(i=>{int at=t.Element(planes,i).At;return new Vector4(t.Vector(at),F(at+12));}).ToArray();
        var faces=new List<byte[]>();int cursor=0;
        for(int i=0;i<24;i++) {
            int count=t.Byte(t.Element(counts,i).At);Need(count==(i<8?4:i<10?10:i<22?3:0),"source face population");
            var face=Enumerable.Range(cursor,count).Select(n=>checked((byte)t.UShort(t.Element(indices,n).At))).ToArray();cursor+=count;
            Need(face.All(n=>n<24)&&face.Distinct().Count()==count,"invalid source bridge face indices");if(count>0)faces.Add(face);
        }
        Need(cursor==88,"source hull connectivity coverage");float radius=F(t.Nested(shape.Type,shape.At,"radius").At);Need(radius==0,"source bridge convex radius");
        var result=new Hull(points,equations,faces.ToArray(),radius);Validate(result);return result;
    }
    internal static void Validate(Hull hull) {
        Need(hull.Vertices.Length==24&&hull.Planes.Length==24&&hull.Faces.Length==22&&hull.Faces.Sum(f=>f.Length)==88&&hull.Radius==0,"bridge hull population");
        for(int i=0;i<24;i++) {
            var p=hull.Planes[i];var n=new Vector3(p.X,p.Y,p.Z);Need(Math.Abs(n.LengthSquared()-1)<.00001f,"source hull plane normalization");
            // The shipped hull's plane coefficients differ from connected
            // vertices by up to 0.069mm; preserve those source values verbatim.
            Need(hull.Vertices.All(v=>Vector3.Dot(n,v)+p.W<.0001f),"source convex plane excludes a hull vertex");
            if(i<22)Need(hull.Faces[i].All(v=>Math.Abs(Vector3.Dot(n,hull.Vertices[v])+p.W)<.0001f),"source face/plane ownership");
        }
        // A closed oriented polytope has each undirected edge exactly twice.
        var edges=hull.Faces.SelectMany(f=>Enumerable.Range(0,f.Length).Select(i=>(A:Math.Min(f[i],f[(i+1)%f.Length]),B:Math.Max(f[i],f[(i+1)%f.Length]))));
        Need(edges.GroupBy(e=>e).All(g=>g.Count()==2),"source bridge hull is not closed");
    }
    // This quantization matches every one of 7,228 connected faces from 500
    // shipped BB object resources, including arbitrary non-box convex solids.
    internal static byte FaceAngle(Hull h,int face) {
        var polygon=h.Faces[face];float dot=-1;int neighbors=0;
        for(int j=0;j<h.Faces.Length;j++)if(j!=face&&h.Faces[j].Intersect(polygon).Count()>=2) {
            dot=Math.Max(dot,Vector3.Dot(new(h.Planes[face].X,h.Planes[face].Y,h.Planes[face].Z),new(h.Planes[j].X,h.Planes[j].Y,h.Planes[j].Z)));neighbors++;
        }
        Need(neighbors>=3,"bridge face has incomplete adjacency");
        return checked((byte)Math.Round(Math.Clamp(Math.Acos(Math.Clamp(dot,-1,1)),0,Math.PI/2)*255/Math.PI));
    }
    internal static byte[] Convert(byte[] native,Hull hull) {
        Validate(hull);Need(System.Convert.ToHexStringLower(SHA256.HashData(native))=="6d5dc4832134b04c40902240c2ad26721c71a90bb35f36d81e96fe94f7b30c6d","unverified one-body native carrier");
        var pack=new NativeAnimation.Pack(native);const int oldShape=720;Need(pack.Data==512&&pack.Object("hknpConvexPolytopeShape")==oldShape,"native shape offset");
        int header=64+BitConverter.ToUInt16(native,62)+128,data=pack.Data;
        int I(int at)=>BitConverter.ToInt32(native,at);int oldLocal=I(header+24),oldGlobal=I(header+28),oldVirtual=I(header+32),oldEnd=I(header+36);
        Need(I(header+40)==oldEnd&&I(header+44)==oldEnd&&data+oldEnd==native.Length,"unexpected native object imports/exports/trailer");
        var body=native.AsSpan(data,oldLocal).ToArray().ToList();while(body.Count%16!=0)body.Add(0);int shape=body.Count;
        var shapeBytes=new byte[1008];native.AsSpan(data+oldShape,64).CopyTo(shapeBytes);
        void U16(int at,int n)=>BitConverter.GetBytes(checked((ushort)n)).CopyTo(shapeBytes,at);
        void U32(int at,uint n)=>BitConverter.GetBytes(n).CopyTo(shapeBytes,at);
        void F(int at,float n)=>BitConverter.GetBytes(n).CopyTo(shapeBytes,at);
        void Relative(int field,int count,int target){U16(field,count);U16(field+2,target-field);}
        const int va=64,pa=448,fa=832,ix=920;
        Relative(40,24,va);Relative(44,24,pa);Relative(48,22,fa);Relative(52,88,ix);F(20,0);
        for(int i=0;i<24;i++) {
            var v=hull.Vertices[i];F(va+i*16,v.X);F(va+i*16+4,v.Y);F(va+i*16+8,v.Z);U32(va+i*16+12,0x3f000000u|(uint)i);
            var p=hull.Planes[i];F(pa+i*16,p.X);F(pa+i*16+4,p.Y);F(pa+i*16+8,p.Z);F(pa+i*16+12,p.W);
        }
        int cursor=0;for(int i=0;i<22;i++){var f=hull.Faces[i];U16(fa+i*4,cursor);shapeBytes[fa+i*4+2]=checked((byte)f.Length);shapeBytes[fa+i*4+3]=FaceAngle(hull,i);f.CopyTo(shapeBytes,ix+cursor);cursor+=f.Length;}
        body.AddRange(shapeBytes);while(body.Count%16!=0)body.Add(0);int local=body.Count;
        int Field(int at)=>at==oldShape+32?shape+32:at;
        int Target(int at)=>at==oldShape?shape:at;
        var locals=new List<(int Field,int Target)>();for(int at=oldLocal;at+8<=oldGlobal;at+=8)if(I(data+at)!=-1)locals.Add((Field(I(data+at)),Target(I(data+at+4))));
        void Int(int n)=>body.AddRange(BitConverter.GetBytes(n));
        foreach(var p in locals.OrderBy(p=>p.Field)){Int(p.Field);Int(p.Target);}while(body.Count%16!=0)body.Add(255);int global=body.Count;
        int redirected=0;
        for(int at=oldGlobal;at+12<=oldVirtual;at+=12)if(I(data+at)!=-1){int f=I(data+at),section=I(data+at+4),target=I(data+at+8);Need(section==2,"external object shape pointer");if(target==oldShape)redirected++;Int(Field(f));Int(section);Int(Target(target));}
        while(body.Count%16!=0)body.Add(255);int virtuals=body.Count;
        for(int at=oldVirtual;at+12<=oldEnd;at+=12)if(I(data+at)!=-1){Int(Target(I(data+at)));Int(I(data+at+4));Int(I(data+at+8));}
        while(body.Count%16!=0)body.Add(255);int end=body.Count;Need(redirected==2,"body and referenced-shapes ownership changed");
        var output=native.AsSpan(0,data).ToArray().Concat(body).ToArray();int[] offsets=[local,global,virtuals,end,end,end];for(int i=0;i<6;i++)BitConverter.GetBytes(offsets[i]).CopyTo(output,header+24+i*4);
        var read=new NativeAnimation.Pack(output);Need(read.Object("hknpConvexPolytopeShape")==shape,"shape virtual fixup relocation");
        var pointers=Pointers(output);Need(pointers[560]==shape&&pointers[672]==shape&&pointers[shape+32]==Pointers(native)[oldShape+32],"shape property/body ownership lost");return output;
    }
    internal static IReadOnlyDictionary<int,int> Pointers(byte[] bytes) {
        int header=64+BitConverter.ToUInt16(bytes,62)+128,data=BitConverter.ToInt32(bytes,header+20);
        int I(int at)=>BitConverter.ToInt32(bytes,at);int local=I(header+24),global=I(header+28),virtuals=I(header+32);var pointers=new Dictionary<int,int>();
        for(int at=local;at+8<=global;at+=8)if(I(data+at)!=-1)pointers.Add(I(data+at),I(data+at+4));
        for(int at=global;at+12<=virtuals;at+=12)if(I(data+at)!=-1){Need(I(data+at+4)==2,"nonlocal physics pointer");pointers.Add(I(data+at),I(data+at+8));}
        return pointers;
    }
}
