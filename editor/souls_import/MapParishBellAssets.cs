// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text;

// o1202's base collider is fixed to the root; only its handle animates.
internal static class MapParishBellAssets {
    internal sealed record Result(byte[] Bundle,DoorPhysics.Box Box,int Frames,float Duration,string SourceSHA256);
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Parish bell asset: "+why);}
    static string Hash(byte[] b)=>System.Convert.ToHexStringLower(SHA256.HashData(b));
    static float Error(Matrix4x4 a,Matrix4x4 b)=>new[]{Vector3.Zero,Vector3.UnitX,Vector3.UnitY,Vector3.UnitZ}.Max(v=>Vector3.Distance(Vector3.Transform(v,a),Vector3.Transform(v,b)));
    // Read the known native system/body layout without interpreting its several
    // compressed-mesh shape classes. All pointers remain inside section2.
    static void VerifyNativeRootBody(byte[] bytes) {
        Need(Hash(bytes)=="69dea4530869d06a19148ce24ca3a1565c172124d006fb3411c315e0098e3005","native lever physics changed");
        int section=64+BitConverter.ToUInt16(bytes,62),header=section+128,classes=BitConverter.ToInt32(bytes,section+20),data=BitConverter.ToInt32(bytes,header+20);
        int I(int at)=>BitConverter.ToInt32(bytes,at);int local=I(header+24),global=I(header+28),virtuals=I(header+32),end=I(header+36);var pointers=new Dictionary<int,int>();
        for(int at=local;at+8<=global;at+=8)if(I(data+at)!=-1)pointers.Add(I(data+at),I(data+at+4));
        for(int at=global;at+12<=virtuals;at+=12)if(I(data+at)!=-1){Need(I(data+at+4)==2,"native external physics pointer");pointers.Add(I(data+at),I(data+at+8));}
        string Text(int at){int stop=Array.IndexOf(bytes,(byte)0,at);Need(stop>=at&&stop-at<128,"native physics string");return Encoding.ASCII.GetString(bytes,at,stop-at);}
        var systems=new List<int>();for(int at=virtuals;at+12<=end;at+=12)if(I(data+at)!=-1&&I(data+at+4)==0&&Text(classes+I(data+at+8))=="hknpPhysicsSystemData")systems.Add(I(data+at));
        Need(systems.Count==1,"native lever physics system");int system=systems[0];Need(I(data+system+72)==4,"native four-body lever precedent");int bodies=pointers[system+64];
        Need(Enumerable.Range(0,4).Select(i=>Text(data+pointers[bodies+i*96+32])).SequenceEqual(new[]{"o240001","o240001_00","o240001_01","o240001_02"}),"native root/animated body names");
        Need(Enumerable.Range(0,3).All(i=>Math.Abs(BitConverter.ToSingle(bytes,data+bodies+48+i*4))<.000001f)&&Enumerable.Range(0,3).All(i=>Math.Abs(BitConverter.ToSingle(bytes,data+bodies+64+i*4))<.000001f)&&Math.Abs(Math.Abs(BitConverter.ToSingle(bytes,data+bodies+76))-1)<.000001f,"native root body identity transform");
    }
    internal static Result Convert(byte[] sourceBytes,byte[] nativeDoorBytes,byte[] nativeLeverBytes,FLVER2 surface,Func<IFlverMaterial,FLVER2.Material> materialFactory) {
        string hash=Hash(sourceBytes);Need(hash=="b7e1953e78cf2ef5b7ffd834e6babc78f158d7059a46018a7dac6ebd682d0dd4","unverified source o1202");
        var source=BND3.Read(sourceBytes);var output=BND4.Read(nativeDoorBytes);var lever=BND4.Read(nativeLeverBytes);VerifyNativeRootBody(lever.Files.Single(f=>f.ID==300).Bytes.ToArray());
        var authored=FLVER2.Read(source.Files.Single(f=>f.ID==200).Bytes);var donor=FLVER2.Read(output.Files.Single(f=>f.ID==200).Bytes);
        Need(authored.Nodes.Select(n=>n.Name).SequenceEqual(new[]{"o1202","o1202_01"})&&authored.Nodes.Select(n=>(int)n.ParentIndex).SequenceEqual(new[]{-1,0}),"source rigid lever hierarchy");
        var visual=MapBreakables.RetargetVisual(authored,donor,surface,"DS1R",Vector3.One,new Dictionary<int,int>{{0,0},{1,1}},materialFactory);
        visual.Dummies.Clear();foreach(var original in authored.Dummies){Need(original.AttachBoneIndex==-1&&original.ParentBoneIndex==0,"source fixed action marker");visual.Dummies.Add(new FLVER.Dummy(original));}
        Need(visual.Dummies.Single(d=>d.ReferenceID==191).Position.Z>1,"source front interaction marker");output.Files.Single(f=>f.ID==200).Bytes=visual.Write();
        var raw=source.Files.Single(f=>f.ID==300).Bytes.ToArray();var t=new SourceCollision.Tag(raw);var body=t.Items.Single(i=>i.Count==1&&t.Name(i.Type)=="hkpRigidBody");var name=t.Reference(t.Nested(body.Type,body.At,"name").At);
        Need(SourceAnimation.DecodeBoneName(raw.AsSpan(name.At,name.Count-1).ToArray())=="o1202_h"&&t.Transform(t.Nested(body.Type,body.At,"motion","motionState","transform").At)==Matrix4x4.Identity,"source fixed base body");
        var translated=t.Reference(t.Nested(body.Type,body.At,"collidable","shape").At);Need(t.Name(translated.Type)=="hkpConvexTranslateShape","source base translation");var shape=t.Reference(t.Nested(translated.Type,translated.At,"childShape","childShape").At);Need(t.Name(shape.Type)=="hkpBoxShape","source base box");
        var box=new DoorPhysics.Box(t.Vector(t.Nested(shape.Type,shape.At,"halfExtents").At),t.Vector(t.Nested(translated.Type,translated.At,"translation").At),BitConverter.ToSingle(raw,t.Nested(shape.Type,shape.At,"radius").At));
        Need(Vector3.Distance(box.Half,new(.24935097f,.2341388f,.78923965f))<.000001f&&Vector3.Distance(box.Center,new(.003951393f,.23413874f,.028726548f))<.000001f,"source fixed base dimensions");
        var physics=DoorPhysics.Fit(output.Files.Single(f=>f.ID==300).Bytes.ToArray(),box);var p=new NativeAnimation.Pack(physics);int system=p.Object("hknpPhysicsSystemData");var bodies=p.Array(system+64,96,1);Need(bodies.Count==1&&p.String(bodies.At+32)=="o241013_00","native single-body binding");
        int bodyName=p.Pointer(bodies.At+32);Array.Clear(physics,p.Data+bodyName,11);Encoding.ASCII.GetBytes("o241013").CopyTo(physics,p.Data+bodyName);Need(new NativeAnimation.Pack(physics).String(bodies.At+32)==visual.Nodes[0].Name,"fixed collider must bind root, never moving handle");output.Files.Single(f=>f.ID==300).Bytes=physics;
        var sourceAnims=BND3.Read(source.Files.Single(f=>f.ID==400).Bytes);var animations=BND4.Read(output.Files.Single(f=>f.ID==400).Bytes);var nativeAnims=BND4.Read(lever.Files.Single(f=>f.ID==400).Bytes);var rig=NativeAnimation.Skeleton(animations.Files.Single(f=>f.ID==1000000).Bytes.ToArray());
        var clip=SourceAnimation.Decode(sourceAnims.Files.Single(f=>f.ID==1000000).Bytes.ToArray(),sourceAnims.Files.Single(f=>f.ID==0).Bytes.ToArray());Need(clip.Bones.Select(b=>b.Name).SequenceEqual(new[]{"o1202","o1202_01"})&&clip.Bones.Select(b=>b.Parent).SequenceEqual(new[]{-1,0})&&clip.FrameCount==106&&Math.Abs(clip.Duration-3.5f)<.000001f,"source bell lever motion");
        Need(rig.Names.SequenceEqual(new[]{"o241013","o241013_00"}),"native two-node carrier");var reference=clip.Bones[1];var local=Matrix4x4.CreateScale(reference.Scale)*Matrix4x4.CreateFromQuaternion(reference.Rotation)*Matrix4x4.CreateTranslation(reference.Translation);Need(Matrix4x4.Invert(local,out var inverse),"singular source handle");
        foreach(int id in new[]{0,1}){int frames=id==0?2:clip.FrameCount;var samples=new List<Matrix4x4>();for(int f=0;f<frames;f++){var pose=clip.Sample(id==0?0:Math.Min(f*clip.FrameDuration,clip.Duration));Need(Error(pose[0],Matrix4x4.Identity)<.000001f,"source fixed root moved");samples.Add(rig.Local[0]);samples.Add(rig.Local[1]*inverse*pose[1]);}animations.Files.Single(f=>f.ID==1000000000+id).Bytes=NativeSpline.Encode(samples,2,frames,clip.FrameDuration,rig.Name,rig.Names);}
        var nativeClip=new NativeAnimation.Pack(nativeAnims.Files.Single(f=>f.ID==1000000001).Bytes.ToArray());int a=nativeClip.Object("hkaSplineCompressedAnimation");Need(nativeClip.Int(a+56)==106&&nativeClip.Float(a+16)==clip.Duration,"native lever timing precedent");var nativeTae=nativeAnims.Files.Single(f=>f.ID==3000000).Bytes;var tae=TAE.Read(nativeTae);Need(tae.Animations.Select(t=>t.ID).SequenceEqual(new long[]{0,1})&&tae.Animations[1].Events.Count==1&&tae.Animations[1].Events[0].Type==128,"native lever sound/controller TAE");
        animations.Files.Single(f=>f.ID==3000000).Bytes=nativeTae.ToArray();animations.Files.RemoveAll(f=>f.ID is 1000000002 or 1000000003);Need(animations.Files.Count==4,"native two-clip lever closure");output.Files.Single(f=>f.ID==400).Bytes=animations.Write();foreach(var file in output.Files)file.Name=file.Name.Replace("o241013","o696010",StringComparison.Ordinal);
        return new(output.Write(DCX.Type.DCX_DFLT_10000_44_9),box,clip.FrameCount,clip.Duration,hash);
    }
}
