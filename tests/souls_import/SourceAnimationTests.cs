// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;

if(args.Length!=1)throw new ArgumentException("Supply the DS1 Remastered source directory");
string source=Path.GetFullPath(args[0]);
byte[]? Read(string path)=>File.Exists(Path.Combine(source,path))?File.ReadAllBytes(Path.Combine(source,path)):null;
void Assert(bool value,string message){if(!value)throw new Exception(message);}
void Reject(Action action,string message) {
    try{action();}catch(Exception e)when(e is InvalidDataException or NotSupportedException or ArgumentException){return;}
    throw new Exception("Accepted malformed source animation: "+message);
}
float Difference(Matrix4x4 a,Matrix4x4 b)=>Math.Max(Vector3.Distance(a.Translation,b.Translation),
    Math.Max(Vector3.Distance(Vector3.TransformNormal(Vector3.UnitX,a),Vector3.TransformNormal(Vector3.UnitX,b)),
    Math.Max(Vector3.Distance(Vector3.TransformNormal(Vector3.UnitY,a),Vector3.TransformNormal(Vector3.UnitY,b)),Vector3.Distance(Vector3.TransformNormal(Vector3.UnitZ,a),Vector3.TransformNormal(Vector3.UnitZ,b)))));
Matrix4x4[] World(SourceAnimation animation,float time) {
    var local=animation.Sample(time);var world=new Matrix4x4[local.Length];
    Matrix4x4 At(int i)=>world[i].M44==1?world[i]:world[i]=local[i]*(animation.Bones[i].Parent<0?Matrix4x4.Identity:At(animation.Bones[i].Parent));
    for(int i=0;i<world.Length;++i)At(i);return world;
}
int Bone(SourceAnimation animation,string name)=>Enumerable.Range(0,animation.Bones.Count).Single(i=>animation.Bones[i].Name==name);
Vector3 Position(SourceAnimation animation,string name,float time)=>World(animation,time)[Bone(animation,name)].Translation;
var originalHashes=new Dictionary<string,string>();
foreach(string model in new[]{"c2900","c2910"}) {
    string file=$"chr/{model}.anibnd.dcx";originalHashes[file]=Convert.ToHexString(SHA256.HashData(Read(file)!));
    foreach(int id in new[]{9001,9061,2200,2250}) {
        var animation=SourceAnimation.Load(Read,model,id);
        Assert(animation.Bones.Count==59,$"{model} {id}: expected all59 source bones");
        Assert(Math.Abs(animation.FrameDuration*(animation.FrameCount-1)-animation.Duration)<.00001f,"Frame timing changed");
        var first=animation.Sample(0);var last=animation.Sample(animation.Duration);
        for(int sample=0;sample<=101;++sample) {
            float t=animation.Duration*sample/101;var local=animation.Sample(t);
            Assert(local.SequenceEqual(animation.Sample(t)),"Fractional sampling is not deterministic");
            for(int i=0;i<local.Length;++i)Assert(Matrix4x4.Decompose(local[i],out var s,out var q,out var p)&&float.IsFinite(s.LengthSquared())&&float.IsFinite(q.LengthSquared())&&float.IsFinite(p.LengthSquared()),"Non-finite sampled pose");
        }
        if(id==9001)Assert(first.SequenceEqual(last)&&first.SequenceEqual(animation.Sample(animation.Duration*.513f)),"Static bone-pile hold moved");
        if(id==9061) {
            float headStart=Position(animation,"Head",0).Y,headEnd=Position(animation,"Head",animation.Duration).Y;
            Assert(headStart<.15f&&headEnd>(model=="c2900"?1.5f:2.3f),"Awakening did not raise the source bones from the ground");
            Assert(first.Where((p,i)=>Difference(p,last[i])>.1f).Count()>10,"Awakening endpoint wrapped back to its first frame");
            var a=animation.Sample(animation.FrameDuration*.15f);var b=animation.Sample(animation.FrameDuration*.65f);
            Assert(a.Where((p,i)=>Difference(p,b[i])>.0001f).Any(),"Fractional spline time was rounded to a whole frame");
            // Golden source-space head transforms independently checked against
            // Smithbox's original decoder; no game assets are stored in tests.
            var expected=model=="c2900"?new Vector3(.22210371f,.10765466f,-.8823231f):new Vector3(.5496742f,.17737538f,-.31846675f);
            Assert(Vector3.Distance(Position(animation,"Head",animation.Duration/2),expected)<.00002f,"Fractional source spline pose differs from independently decoded source");
            var idle=SourceAnimation.Load(Read,model,0);
            foreach(string name in new[]{"master","Pelvis","L Foot","R Foot","Head"})
                Assert(Math.Abs(Position(animation,name,animation.Duration).Y-Position(idle,name,0).Y)<.0002f,"Source awakening endpoint did not return to idle height for "+name);
        }
        Reject(()=>animation.Sample(float.NaN),"NaN sample");Reject(()=>animation.Sample(-.1f),"negative sample");Reject(()=>animation.Sample(animation.Duration+.1f),"sample past endpoint");
    }
    Console.WriteLine($"PASS {model}: static bone pile, fractional awakening, collapse, pile hold and exact endpoint");
}

foreach(int id in new[]{3006,3007}) {
    var animation=SourceAnimation.Load(Read,"c2900",id);
    Assert(animation.FrameCount>256,"Expected an actual two-block source clip");
    float boundary=255*animation.FrameDuration;
    var before=animation.Sample(boundary-.00001f);var exact=animation.Sample(boundary);var after=animation.Sample(boundary+.00001f);
    // Independently compressed blocks have their own quantization errors: the
    // source3007 toe differs by0.45degrees at this shared frame.
    for(int i=0;i<before.Length;++i)Assert(Difference(before[i],exact[i])<.01f&&Difference(exact[i],after[i])<.01f,"Source spline jumps across its shared block frame");
    var expectedRoot=id==3006?new Vector3(-.007088048f,1.0606999f,-.032374f):new Vector3(-.007000877f,1.066144f,-.032374f);
    Assert(Vector3.Distance(animation.Sample(8.533333f)[Bone(animation,"master")].Translation,expectedRoot)<.000002f,"Second source animation block uses the wrong shared-frame stride");
    for(int n=0;n<=1001;++n)_=animation.Sample(animation.Duration*n/1001);
}
Console.WriteLine("PASS actual two-block source clips: shared frame255 boundary, fractional samples and final endpoint");

var archive=BND3.Read(Read("chr/c2900.anibnd.dcx")!);
byte[] FileBytes(string name)=>archive.Files.Single(f=>f.Name.Replace('\\','/').Split('/').Last().Equals(name,StringComparison.OrdinalIgnoreCase)).Bytes.ToArray();
var skeletonBytes=FileBytes("skeleton.hkx");var animationBytes=FileBytes("a00_9061.hkx");
var clipTag=new SourceCollision.Tag(animationBytes);var anim=clipTag.Items.Single(i=>clipTag.Name(i.Type)=="hkaSplineCompressedAnimation");
var binding=clipTag.Items.Single(i=>clipTag.Name(i.Type)=="hkaAnimationBinding");
int Member(string name)=>clipTag.Nested(anim.Type,anim.At,name).At;
var data=clipTag.Array(anim,"data");int trackCount=clipTag.Int(Member("numberOfTransformTracks"));
void BadInt(int at,int value,string name) {var bytes=animationBytes.ToArray();BitConverter.GetBytes(value).CopyTo(bytes,at);Reject(()=>SourceAnimation.Decode(skeletonBytes,bytes),name);}
void BadByte(int at,byte value,string name) {var bytes=animationBytes.ToArray();bytes[at]=value;Reject(()=>SourceAnimation.Decode(skeletonBytes,bytes),name);}
BadInt(Member("numFrames"),int.MaxValue,"unbounded frame count");
BadInt(Member("numBlocks"),2,"inconsistent block count");
BadInt(Member("duration"),unchecked((int)0x7fc00000),"non-finite duration");
BadInt(Member("numberOfFloatTracks"),1,"float tracks");
BadByte(clipTag.Nested(binding.Type,binding.At,"blendHint").At,1,"additive animation");
var map=clipTag.Array(binding,"transformTrackToBoneIndices");
var reordered=animationBytes.ToArray();int firstBone=clipTag.UShort(clipTag.Element(map,0).At),secondBone=clipTag.UShort(clipTag.Element(map,1).At);
BitConverter.GetBytes((ushort)secondBone).CopyTo(reordered,clipTag.Element(map,0).At);BitConverter.GetBytes((ushort)firstBone).CopyTo(reordered,clipTag.Element(map,1).At);
var originalPose=SourceAnimation.Decode(skeletonBytes,animationBytes).Sample(1.3f);var reorderedPose=SourceAnimation.Decode(skeletonBytes,reordered).Sample(1.3f);
Assert(Math.Abs(originalPose[firstBone].Translation.Y-reorderedPose[secondBone].Translation.Y)<.000001f&&Math.Abs(originalPose[secondBone].Translation.Y-reorderedPose[firstBone].Translation.Y)<.000001f,"Transform tracks ignored the source binding map");
var decodedReference=SourceAnimation.Decode(skeletonBytes,animationBytes);
int missingPositionChecks=0;
for(int i=0;i<trackCount;++i)if(clipTag.Byte(data.At+i*4+1)==0) {
    int bone=clipTag.UShort(clipTag.Element(map,i).At);Assert(originalPose[bone].Translation==decodedReference.Bones[bone].Translation,"Missing source track components did not preserve the reference pose");++missingPositionChecks;
}
Assert(missingPositionChecks>0,"Expected source tracks with reference-pose fallback");
BadByte(clipTag.Element(map,1).At,clipTag.Byte(clipTag.Element(map,0).At),"duplicate bone binding");
BadInt(clipTag.Element(clipTag.Array(anim,"blockOffsets"),0).At,4,"misaligned initial block offset");
BadInt(clipTag.Element(clipTag.Array(anim,"floatBlockOffsets"),0).At,data.Count+1,"float block endpoint outside data");
BadByte(data.At,0x4d,"unsupported rotation quantization");
int firstCurve=data.At+((trackCount*4+3)/4*4);
BadInt(firstCurve,32767,"unbounded spline controls");
BadByte(firstCurve+2,5,"unbounded spline degree");
BadByte(firstCurve+3,255,"unordered spline knots");
int count=clipTag.UShort(firstCurve)+1,degree=clipTag.Byte(firstCurve+2),bounds=(firstCurve-data.At+3+count+degree+1+3)/4*4+data.At;
BadInt(bounds,unchecked((int)0x7fc00000),"non-finite spline bounds");
var skeletonTag=new SourceCollision.Tag(skeletonBytes);var sk=skeletonTag.Items.Single(i=>skeletonTag.Name(i.Type)=="hkaSkeleton");
var parents=skeletonTag.Array(sk,"parentIndices");var cyclic=skeletonBytes.ToArray();
BitConverter.GetBytes((short)1).CopyTo(cyclic,skeletonTag.Element(parents,0).At);BitConverter.GetBytes((short)0).CopyTo(cyclic,skeletonTag.Element(parents,1).At);
Reject(()=>SourceAnimation.Decode(cyclic,animationBytes),"cyclic skeleton");
Reject(()=>SourceAnimation.Decode(skeletonBytes,animationBytes[..^1]),"truncated source tagfile");
Reject(()=>SourceAnimation.Load(Read,"../c2900",9061),"source identifier traversal");
Reject(()=>SourceAnimation.Load(Read,"c2900",10000),"source clip identifier out of range");
Assert(animationBytes.SequenceEqual(FileBytes("a00_9061.hkx"))&&skeletonBytes.SequenceEqual(FileBytes("skeleton.hkx")),"Decoding changed source byte buffers");
foreach(var(path,hash) in originalHashes)Assert(hash==Convert.ToHexString(SHA256.HashData(Read(path)!)),"Source archive changed");
Console.WriteLine("PASS malformed source metadata, bindings, spline bounds, hierarchy and source preservation");
