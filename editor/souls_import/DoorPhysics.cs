// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;

internal static class DoorPhysics {
    internal record Box(Vector3 Half,Vector3 Center,float Radius);
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Door collision: "+why);}
    internal static Box Source(byte[] bytes) {
        var tag=new SourceCollision.Tag(bytes);SourceCollision.Tag.Item Named(string name)=>tag.Items.Single(i=>tag.Name(i.Type)=="hkpRigidBody"&&Text(i)==name);
        string Text(SourceCollision.Tag.Item i){var n=tag.Reference(tag.Nested(i.Type,i.At,"name").At);return SourceAnimation.DecodeBoneName(Enumerable.Range(n.At,n.Count-1).Select(tag.Byte).ToArray());}
        var body=Named("o1305_01");var translated=tag.Reference(tag.Nested(body.Type,body.At,"collidable","shape").At);
        Need(tag.Name(translated.Type)=="hkpConvexTranslateShape","source leaf is not a translated box");
        var shape=tag.Reference(tag.Nested(translated.Type,translated.At,"childShape","childShape").At);Need(tag.Name(shape.Type)=="hkpBoxShape","source leaf box changed");
        var half=tag.Vector(tag.Nested(shape.Type,shape.At,"halfExtents").At);var center=tag.Vector(tag.Nested(translated.Type,translated.At,"translation").At);
        float radius=BitConverter.UInt32BitsToSingle(tag.UInt(tag.Nested(shape.Type,shape.At,"radius").At));
        Need(Vector3.Distance(half,new(.6901112f,1.1768674f,.012505652f))<.00001f&&Vector3.Distance(center,new(.7026058f,1.189373f,-.025013994f))<.00001f&&Math.Abs(radius-.012505652f)<.00001f,"authored source door box changed");
        return new(half,center,radius);
    }
    internal static byte[] Fit(byte[] input,Box box) {
        // This retail single-leaf physics resource has one keyframed body,
        // named exactly like the native animated leaf. Keep its body/name,
        // filter, material, fixups and inline convex topology intact.
        Need(Convert.ToHexStringLower(SHA256.HashData(input))=="6d5dc4832134b04c40902240c2ad26721c71a90bb35f36d81e96fe94f7b30c6d","native single-leaf collision contract changed");
        var p=new NativeAnimation.Pack(input);Need(p.Data==512&&p.Object("hknpConvexPolytopeShape")==720,"native door box layout changed");
        const int shape=720,vertices=shape+64,planes=shape+192;var output=input.ToArray();
        Vector3 V(int at)=>new(p.Float(at),p.Float(at+4),p.Float(at+8));
        var old=Enumerable.Range(0,8).Select(i=>V(vertices+i*16)).ToArray();var oldCenter=(old.Aggregate(Vector3.Min)+old.Aggregate(Vector3.Max))*.5f;
        void Float(int at,float f)=>BitConverter.GetBytes(f).CopyTo(output,p.Data+at);
        for(int i=0;i<8;i++){var v=old[i];var h=box.Half;var c=box.Center;Float(vertices+i*16,c.X+(v.X<oldCenter.X?-h.X:h.X));Float(vertices+i*16+4,c.Y+(v.Y<oldCenter.Y?-h.Y:h.Y));Float(vertices+i*16+8,c.Z+(v.Z<oldCenter.Z?-h.Z:h.Z));}
        for(int i=0;i<6;i++){var normal=V(planes+i*16);Float(planes+i*16+12,-Vector3.Dot(normal,box.Center)-Math.Abs(normal.X)*box.Half.X-Math.Abs(normal.Y)*box.Half.Y-Math.Abs(normal.Z)*box.Half.Z);}
        Float(shape+20,box.Radius);return output;
    }
}
