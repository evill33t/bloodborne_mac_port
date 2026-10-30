// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;

if(args.Length!=2)throw new ArgumentException("Supply DS1 Remastered and Bloodborne dvdroot_ps4 directories");
string source=Path.GetFullPath(args[0]),native=Path.GetFullPath(args[1]);
byte[]? Read(string path)=>File.Exists(Path.Combine(source,path))?File.ReadAllBytes(Path.Combine(source,path)):null;
byte[] Native(string path)=>File.ReadAllBytes(Path.Combine(native,path));
string Leaf(string path)=>path.Replace('\\','/').Split('/').Last();
string Hash(byte[] bytes)=>Convert.ToHexString(SHA256.HashData(bytes));
void Assert(bool value,string message){if(!value)throw new Exception(message);}
void Reject(Action action,string message) {
    try{action();}catch(Exception e)when(e is InvalidDataException or NotSupportedException or ArgumentException){return;}
    throw new Exception("Accepted malformed native animation: "+message);
}
Matrix4x4[] World(IReadOnlyList<Matrix4x4> local,IReadOnlyList<int> parents) {
    var result=new Matrix4x4[local.Count];var done=new bool[local.Count];
    Matrix4x4 At(int i){if(done[i])return result[i];result[i]=parents[i]<0?local[i]:local[i]*At(parents[i]);done[i]=true;return result[i];}
    for(int i=0;i<local.Count;++i)At(i);return result;
}
Matrix4x4[] SourceRest(SourceAnimation animation)=>animation.Bones.Select(b=>Matrix4x4.CreateScale(b.Scale)*Matrix4x4.CreateFromQuaternion(b.Rotation)*Matrix4x4.CreateTranslation(b.Translation)).ToArray();
float Difference(Matrix4x4 a,Matrix4x4 b)=>new[]{Vector3.Zero,Vector3.UnitX,Vector3.UnitY,Vector3.UnitZ}.Max(p=>Vector3.Distance(Vector3.Transform(p,a),Vector3.Transform(p,b)));
void QuantizedTransform(Matrix4x4 actual,Matrix4x4 expected,string message) {
    Assert(Matrix4x4.Decompose(actual,out var aScale,out var aRotation,out var aPosition)&&Matrix4x4.Decompose(expected,out _,out _,out _),message+": cannot decompose");
    Matrix4x4.Decompose(expected,out var eScale,out var eRotation,out var ePosition);
    float angle=2*MathF.Acos(Math.Clamp(Math.Abs(Quaternion.Dot(Quaternion.Normalize(aRotation),Quaternion.Normalize(eRotation))),0,1));
    Assert(angle<.005f&&Vector3.Distance(aPosition,ePosition)<.00003f&&Vector3.Distance(aScale,eScale)<.00003f,message+": quantization exceeds native error bounds");
}

// Construct an animation fixture without a binary encoder: zero tracks means
// the source reference pose; one static root track adds a known translation.
SourceAnimation ReferenceClip(SourceAnimation sourceAnimation,Vector3? rootTranslation=null) {
    var owner=typeof(SourceAnimation);var flags=BindingFlags.Instance|BindingFlags.Public|BindingFlags.NonPublic;
    object New(Type type,params object?[] values)=>Activator.CreateInstance(type,flags,null,values,null)!;
    var track=owner.GetNestedType("Track",BindingFlags.NonPublic)!;
    var tracks=System.Array.CreateInstance(track,rootTranslation.HasValue?1:0);int[] map=[];
    if(rootTranslation is {} delta) {
        var vt=owner.GetNestedType("VectorTrack",BindingFlags.NonPublic)!;int root=Enumerable.Range(0,sourceAnimation.Bones.Count).Single(i=>sourceAnimation.Bones[i].Parent<0);
        var p=sourceAnimation.Bones[root].Translation+delta;var position=New(vt,null,new float[]?[]{[p.X],[p.Y],[p.Z]});var scale=New(vt,null,new float[]?[3]);
        tracks.SetValue(New(track,position,null,scale),0);map=[root];
    }
    var blocks=System.Array.CreateInstance(track.MakeArrayType(),1);blocks.SetValue(tracks,0);
    return (SourceAnimation)New(owner,sourceAnimation.Bones.ToArray(),1f,1f,2,256,map,blocks);
}

var sampledFixture=new List<Matrix4x4>();
for(int frame=0;frame<3;++frame)for(int track=0;track<2;++track) {
    var scale=track==0?Vector3.One:new Vector3(1+frame*.1f,.8f+frame*.2f,1.1f);
    sampledFixture.Add(Matrix4x4.CreateScale(scale)*Matrix4x4.CreateRotationY((170+frame*10)*MathF.PI/180)*Matrix4x4.CreateTranslation(frame*.123f,track+frame*.52f,-frame*.277f));
}
var fixturePack=new RawPack(NativeSpline.Encode(sampledFixture,2,3,1f/30,"FixtureSkeleton",["Root","Detail"]));var fixtureClip=fixturePack.Animation(3,2);
Assert(fixturePack.Binding("FixtureSkeleton",fixtureClip.Object,2),"Native spline fixture binding changed");
Assert(fixturePack.TrackNames(fixtureClip.Object).SequenceEqual(new[]{"Root","Detail"}),"Native spline lost its named annotation tracks");
for(int frame=0;frame<3;++frame)for(int track=0;track<2;++track)QuantizedTransform(fixturePack.Pose(fixtureClip,frame)[track],sampledFixture[frame*2+track],"Native spline sampled transform");
var fractionalExpected=Matrix4x4.CreateScale(1.05f,.9f,1.1f)*Matrix4x4.CreateRotationY(175*MathF.PI/180)*Matrix4x4.CreateTranslation(.0615f,1.26f,-.1385f);
QuantizedTransform(fixturePack.FractionalPose(fixtureClip,.5f)[1],fractionalExpected,"Native linear spline fractional pose");
var quaternionFixture=new[]{Quaternion.Normalize(new Quaternion(1.02f,-1,0,.02f)),Quaternion.Normalize(new Quaternion(1,-1.02f,0,.02f)),Quaternion.CreateFromAxisAngle(Vector3.UnitZ,170*MathF.PI/180),Quaternion.Identity,Quaternion.CreateFromAxisAngle(Vector3.UnitX,170*MathF.PI/180),Quaternion.CreateFromAxisAngle(Vector3.UnitY,170*MathF.PI/180)}.Select(Matrix4x4.CreateFromQuaternion).ToArray();
var quaternionPack=new RawPack(NativeSpline.Encode(quaternionFixture,1,quaternionFixture.Length,.01f,"FixtureSkeleton",["Root"]));var quaternionClip=quaternionPack.Animation(quaternionFixture.Length,1);
for(int frame=0;frame<quaternionFixture.Length;++frame)QuantizedTransform(quaternionPack.Pose(quaternionClip,frame)[0],quaternionFixture[frame],"THREECOMP40 largest-component/sign fixture");
var maximumFixture=Enumerable.Repeat(Matrix4x4.Identity,256).ToArray();var maximumPack=new RawPack(NativeSpline.Encode(maximumFixture,1,256,.01f,"FixtureSkeleton",["Master"]));
Assert(maximumPack.Pose(maximumPack.Animation(256,1),255)[0]==Matrix4x4.Identity,"Native spline cannot encode its maximum byte knot endpoint");
Reject(()=>NativeSpline.Encode(maximumFixture,1,257,.01f,"FixtureSkeleton",["Master"]),"multiple blocks beyond verified encoder scope");
Reject(()=>NativeSpline.Encode(sampledFixture,0,3,.01f,"FixtureSkeleton",[]),"zero transform tracks");
Reject(()=>NativeSpline.Encode(sampledFixture,2,3,float.NaN,"FixtureSkeleton",["Root","Detail"]),"non-finite spline duration");
Reject(()=>NativeSpline.Encode(sampledFixture,2,3,.01f,"FixtureSkeleton",["Root"]),"missing native track names");
Reject(()=>NativeSpline.Encode(sampledFixture,2,3,.01f,"FixtureSkeleton",["Root","Root"]),"duplicate native track names");
var nonFinite=sampledFixture.ToArray();nonFinite[0].M44=float.NaN;
Reject(()=>NativeSpline.Encode(nonFinite,2,3,.01f,"FixtureSkeleton",["Root","Detail"]),"non-finite affine matrix component");
Console.WriteLine("PASS native spline writer: independent byte decoding, sampled positions/rotation/scales, hemisphere continuity and maximum knot255");

var nativeAnimationArchive=Native("chr/c2610.anibnd.dcx");var nativeModelArchive=Native("chr/c2610.chrbnd.dcx");
string nativeAnimationHash=Hash(nativeAnimationArchive),nativeModelHash=Hash(nativeModelArchive);
var skeleton=BND4.Read(nativeAnimationArchive).Files.Single(f=>f.ID==1000000).Bytes.ToArray();string skeletonHash=Hash(skeleton);
var nativeFlver=BND4.Read(nativeModelArchive).Files.Single(f=>Leaf(f.Name).Equals("c2610.flver",StringComparison.OrdinalIgnoreCase)).Bytes.ToArray();
var nativeRig=RawPack.Skeleton(skeleton);Assert(nativeRig.Name=="Master"&&nativeRig.Names.Length==80,"Native c2610 skeleton identity changed");
int nativeRoot=Enumerable.Range(0,nativeRig.Names.Length).Single(i=>nativeRig.Parents[i]<0);
var nativeRestWorld=World(nativeRig.Local,nativeRig.Parents);
var originalNativeClip=new RawPack(BND4.Read(nativeAnimationArchive).Files.Single(f=>f.ID==1000007004).Bytes.ToArray());
var originalHeader=originalNativeClip.Header();
Assert(originalHeader.Type==3&&originalHeader.Duration==.5f&&originalHeader.Tracks==80&&originalHeader.FloatTracks==0,"Original native animation tail-padding field offsets changed");
Assert(originalNativeClip.Binding("Master",originalNativeClip.Objects["hkaSplineCompressedAnimation"],80),"Original native binding/container layout differs from export");
Assert(originalNativeClip.TrackNames(originalNativeClip.Objects["hkaSplineCompressedAnimation"]).SequenceEqual(nativeRig.Names),"Original native static annotation names differ from rig order");
Assert(originalNativeClip.ReferenceFrame(originalNativeClip.Objects["hkaSplineCompressedAnimation"])==null,"Native static7004 unexpectedly has extracted motion");
foreach(int slot in new[]{7004,7005,2014,2020,2040}) {
    var original=new RawPack(BND4.Read(nativeAnimationArchive).Files.Single(f=>f.ID==1000000000+slot).Bytes.ToArray());var timing=original.Timing();
    Assert(timing.MaximumFrames==256&&Math.Abs(timing.BlockDuration-255*timing.FrameDuration)<.0001f&&Math.Abs(timing.Inverse*timing.BlockDuration-1)<.00001f,"Original native spline does not use its full block-frame interval");
    Assert(Math.Abs(original.Header().Duration*timing.Inverse*255-(timing.Frames-1))<.0001f,"Native block-time normalization does not reach the final frame");
    if(slot!=7004) {
        var motion=original.ReferenceFrame(original.Objects["hkaSplineCompressedAnimation"])!;
        Assert(motion.Up==new Vector4(0,1,0,0)&&motion.Forward==new Vector4(0,0,1,0)&&motion.Samples.Length==timing.Frames&&Math.Abs(motion.Duration-original.Header().Duration)<.000001f,"Original native reference-frame field layout differs from export");
    }
}
foreach(string model in new[]{"c2900","c2910"}) {
    string animationPath=$"chr/{model}.anibnd.dcx",modelPath=$"chr/{model}.chrbnd.dcx";
    string sourceAnimationHash=Hash(Read(animationPath)!),sourceModelHash=Hash(Read(modelPath)!);
    var sourceFlver=BND3.Read(Read(modelPath)!).Files.Single(f=>Leaf(f.Name).Equals(model+".flver",StringComparison.OrdinalIgnoreCase)).Bytes.ToArray();
    // Mapping is obtained from the actual authored geometry. Texture pixels are
    // irrelevant to clip conversion; small valid fixtures keep this test focused.
    var textures=FLVER2.Read(sourceFlver).Materials.SelectMany(m=>m.Textures).Select(t=>Leaf(t.Path).Split('.')[0]).Distinct(StringComparer.OrdinalIgnoreCase)
        .Select(name=>new TPF.Texture(name,9,0,SkinRetarget.RgbaTexture([255,255,255,255]),TPF.TPFPlatform.PC)).ToArray();
    var skin=SkinRetarget.Bake(sourceFlver,nativeFlver,textures,sourceProfile:"DS1R",fitSegments:true,allowLargeHumanoid:true);
    float stature=SkinRetarget.HumanoidScale(FLVER2.Read(sourceFlver).Nodes,skin.Model.Nodes,true);
    Assert(skin.Bones.Count(b=>b.Direct)>=20,"Expected the actual skeleton appearance mapping");
    foreach(int id in new[]{9001,9061,2014,2020,2040}) {
        var clip=SourceAnimation.Load(Read,model,id);var bytes=NativeAnimation.Convert(clip,skeleton,skin.Bones,stature);var output=new RawPack(bytes);
        var decoded=output.Animation(clip.FrameCount,nativeRig.Names.Length);
        Assert(output.Binding(nativeRig.Name,decoded.Object,nativeRig.Names.Length),"Export changed skeleton identity or transform bindings");
        Assert(output.TrackNames(decoded.Object).SequenceEqual(nativeRig.Names),"Converted animation annotation names do not match native bone order");
        Assert(Math.Abs(decoded.Duration-clip.Duration)<.000001f,"Converted animation duration differs from source");
        Assert(!output.Objects.ContainsKey("hkaSkeleton"),"Animation unexpectedly embedded a replacement native rig");
        for(int frame=0;frame<clip.FrameCount;++frame)_=output.Pose(decoded,frame);
        var sourceParents=clip.Bones.Select(b=>b.Parent).ToArray();var sourceRestWorld=World(SourceRest(clip),sourceParents);
        var sourceIndices=clip.Bones.Select((b,i)=>(b.Name,i)).ToDictionary(v=>v.Name,v=>v.i,StringComparer.Ordinal);
        var targetIndices=nativeRig.Names.Select((name,i)=>(name,i)).ToDictionary(v=>v.name,v=>v.i,StringComparer.Ordinal);
        var sourceRoot=clip.Bones.Single(b=>b.Parent<0);
        Assert(!skin.Bones.Any(b=>b.Source==sourceRoot.Name),"Expected an unweighted source locomotion root absent from mesh mappings");
        var expectedMappings=skin.Bones.Append(new BoneMatch(sourceRoot.Name,nativeRig.Names[nativeRoot],true));
        var sampleFrames=new[]{0,clip.FrameCount/2,clip.FrameCount-1}.Distinct();
        foreach(int frame in sampleFrames) {
            float time=frame==clip.FrameCount-1?clip.Duration:frame*clip.FrameDuration;
            var sourceWorld=World(clip.Sample(time),sourceParents);var targetWorld=World(output.Pose(decoded,frame),nativeRig.Parents);
            foreach(var m in expectedMappings.Where(m=>m.Direct&&sourceIndices.ContainsKey(m.Source)&&targetIndices.ContainsKey(m.Target))) {
                int s=sourceIndices[m.Source],t=targetIndices[m.Target];
                // A target reference-space point first enters the source joint's
                // reference frame, then follows that source joint's sampled pose.
                // Compare transformed points with decoded, serialized native QS
                // data, independently of converter matrix construction.
                var rest=sourceRestWorld[s];rest.Translation/=stature;Assert(Matrix4x4.Invert(rest,out var restInverse),"Singular source test reference");
                var pose=sourceWorld[s];pose.Translation/=stature;
                foreach(var point in new[]{Vector3.Zero,new Vector3(.07f,.13f,-.11f)}) {
                    var expected=Vector3.Transform(Vector3.Transform(Vector3.Transform(point,nativeRestWorld[t]),restInverse),pose);
                    // Forty-bit joint rotations accumulate along the skeleton;
                    // measured worst-case fingertip displacement is4.72mm.
                    var actual=Vector3.Transform(point,targetWorld[t]);Assert(Vector3.Distance(actual,expected)<.006f,$"{model}/{id} bone{m.Source} lost the source joint's pose or translation");
                }
            }
        }
        if(id==9001) {
            Assert(output.Pose(decoded,0).SequenceEqual(output.Pose(decoded,clip.FrameCount-1)),"Native bone-pile hold changed across its frames");
            Assert(Math.Abs(World(output.Pose(decoded,0),nativeRig.Parents)[nativeRoot].Translation.Y)<.2f,"Unweighted native Master stayed at standing reference height during the bone-pile hold");
        }
        if(id==9061) {
            int head=System.Array.IndexOf(nativeRig.Names,skin.Bones.Single(m=>m.Source=="Head").Target);
            float low=World(output.Pose(decoded,0),nativeRig.Parents)[head].Translation.Y;
            float high=World(output.Pose(decoded,clip.FrameCount-1),nativeRig.Parents)[head].Translation.Y;
            Assert(high-low>.7f,"Converted awakening did not assemble the head above the bone pile");
            var firstWorld=World(output.Pose(decoded,0),nativeRig.Parents);var finalWorld=World(output.Pose(decoded,clip.FrameCount-1),nativeRig.Parents);
            Assert(finalWorld[nativeRoot].Translation.Y-firstWorld[nativeRoot].Translation.Y>.8f,"Unweighted Master did not follow the source awakening trajectory");
            int pelvis=targetIndices[skin.Bones.Single(m=>m.Source=="Pelvis").Target];
            Assert(Math.Abs(finalWorld[nativeRoot].Translation.Y-finalWorld[pelvis].Translation.Y)<.0001f,"Awakened Master and pelvis heights diverge");
            var explicitRoot=NativeAnimation.Convert(clip,skeleton,expectedMappings.ToArray(),stature);
            Assert(bytes.SequenceEqual(explicitRoot),"Implicit unweighted-root mapping differs from an explicit mapping");
        }
    }
    var baseClip=SourceAnimation.Load(Read,model,9061);
    var restOutput=new RawPack(NativeAnimation.Convert(ReferenceClip(baseClip),skeleton,skin.Bones,stature));var restData=restOutput.Animation(2,80);
    var restWorld=World(restOutput.Pose(restData,0),nativeRig.Parents);
    for(int i=0;i<80;++i)Assert(Difference(restWorld[i],nativeRestWorld[i])<.004f,"Source reference pose did not preserve native rest bone "+nativeRig.Names[i]);
    var delta=new Vector3(.31f,-.23f,.41f);var shiftedOutput=new RawPack(NativeAnimation.Convert(ReferenceClip(baseClip,delta),skeleton,skin.Bones,stature));var shiftData=shiftedOutput.Animation(2,80);
    var shifted=World(shiftedOutput.Pose(shiftData,1),nativeRig.Parents);
    for(int i=0;i<80;++i) {
        Assert(Vector3.Distance(shifted[i].Translation-restWorld[i].Translation,delta/stature)<.00002f,"Native skeleton lost or incorrectly scaled source root translation");
    }
    foreach(float scale in new[]{float.NaN,float.PositiveInfinity,0f,.1f,2.6f})Reject(()=>NativeAnimation.Convert(baseClip,skeleton,skin.Bones,scale),"invalid stature scale");
    Reject(()=>NativeAnimation.Convert(baseClip,skeleton,[],stature),"missing bone mappings");
    var conflicting=skin.Bones.Concat([new BoneMatch("Pelvis",skin.Bones.Single(m=>m.Source=="Head").Target,true)]).ToArray();
    Reject(()=>NativeAnimation.Convert(baseClip,skeleton,conflicting,stature),"conflicting target bone mappings");
    var rootConflict=skin.Bones.Concat([new BoneMatch("Pelvis",nativeRig.Names[nativeRoot],true)]).ToArray();
    Reject(()=>NativeAnimation.Convert(baseClip,skeleton,rootConflict,stature),"conflicting unweighted-root mapping");
    Assert(Hash(Read(animationPath)!)==sourceAnimationHash&&Hash(Read(modelPath)!)==sourceModelHash,"Conversion changed original source archives");
    Console.WriteLine($"PASS {model}: five native clip round trips, identity bindings, all finiteQS, source-scatter poses, reference preservation and root translation");
}

var nativeReader=new RawPack(skeleton);var skeletonObject=nativeReader.Objects["hkaSkeleton"];
var parentArray=nativeReader.Array(skeletonObject+24,2,256);var referenceArray=nativeReader.Array(skeletonObject+56,48,256);
var sourceClip=SourceAnimation.Load(Read,"c2900",9061);
var allMappings=sourceClip.Bones.Select(b=>new BoneMatch(b.Name,nativeRig.Names.FirstOrDefault(n=>n.Equals(b.Name,StringComparison.OrdinalIgnoreCase))??"",true)).ToArray();
void BadSkeleton(int absolute,byte[] replacement,string message) {var bytes=skeleton.ToArray();replacement.CopyTo(bytes,absolute);Reject(()=>NativeAnimation.Convert(sourceClip,bytes,allMappings,1),message);}
BadSkeleton(16,[4],"wrong native pointer width");BadSkeleton(12,BitConverter.GetBytes(10),"wrong packfile version");
BadSkeleton(62,BitConverter.GetBytes(ushort.MaxValue),"out-of-range section headers");
BadSkeleton(nativeReader.Data+referenceArray.At+16,BitConverter.GetBytes(float.NaN),"non-finite native reference quaternion");
BadSkeleton(nativeReader.Data+parentArray.At,BitConverter.GetBytes((short)1),"cyclic native skeleton hierarchy");
Reject(()=>NativeAnimation.Convert(sourceClip,skeleton[..^1],allMappings,1),"truncated native packfile");
Assert(Hash(skeleton)==skeletonHash&&Hash(Native("chr/c2610.anibnd.dcx"))==nativeAnimationHash&&Hash(Native("chr/c2610.chrbnd.dcx"))==nativeModelHash,"Conversion changed native source assets");
Console.WriteLine("PASS malformed native skeletons, invalid scales and mappings; source/native byte preservation");

// Independent, read-only packfile inspector. It resolves the actual on-disk
// section/fixup records rather than relying on NativeAnimation.Pack or HKX2's
// unimplemented signed-short/QS array readers.
sealed class RawPack {
    readonly byte[] bytes;readonly (int At,int Local,int Global,int Virtual,int End)[] sections=new (int,int,int,int,int)[3];
    readonly Dictionary<int,int> local=[];readonly Dictionary<int,(int Section,int At)> global=[];
    internal readonly Dictionary<string,int> Objects=new(StringComparer.Ordinal);
    internal int Data=>sections[2].At;
    public RawPack(byte[] source) {
        bytes=source;Check(U(0)==0x57e0e057&&U(4)==0x10c0c010&&I(12)==11&&bytes[16]==8&&bytes[17]==1&&I(20)==3,"Unexpected native pack header");
        Check(Encoding.ASCII.GetString(bytes,40,14).StartsWith("hk_2014.1.0-r1",StringComparison.Ordinal),"Unexpected native pack SDK identity");
        int headers=64+S(62);
        for(int n=0;n<3;++n){int at=headers+n*64;var section=(At:I(at+20),Local:I(at+24),Global:I(at+28),Virtual:I(at+32),End:I(at+36));Check(section.At>=0&&section.Local>=0&&section.Global>=section.Local&&section.Virtual>=section.Global&&section.End>=section.Virtual&&section.At<=bytes.Length-section.End,"Invalid native section range");sections[n]=section;}
        var data=sections[2];
        for(int at=Data+data.Local;at+8<=Data+data.Global;at+=8)if(U(at)!=uint.MaxValue){int from=I(at),to=I(at+4);Check(from>=0&&from<=data.Local-8&&to>=0&&to<data.Local,"Invalid local fixup");local.Add(from,to);}
        for(int at=Data+data.Global;at+12<=Data+data.Virtual;at+=12)if(U(at)!=uint.MaxValue){int from=I(at),section=I(at+4),to=I(at+8);Check(from>=0&&from<=data.Local-8&&section is >=0 and <3&&to>=0&&to<sections[section].Local,"Invalid global fixup");global.Add(from,(section,to));}
        for(int at=Data+data.Virtual;at+12<=Data+data.End;at+=12)if(U(at)!=uint.MaxValue){int obj=I(at),section=I(at+4),name=I(at+8);Check(section==0&&obj>=0&&obj<data.Local&&name>=0&&name<sections[0].Local,"Invalid virtual fixup");Objects.Add(Text(sections[0].At+name),obj);}
    }
    static void Check(bool value,string message){if(!value)throw new Exception(message);}
    void Range(int at,int size){Check(at>=0&&size>=0&&at<=bytes.Length-size,"Truncated packfile read");}
    uint U(int at){Range(at,4);return BitConverter.ToUInt32(bytes,at);}
    int I(int at)=>unchecked((int)U(at));
    ushort S(int at){Range(at,2);return BitConverter.ToUInt16(bytes,at);}
    float F(int at){float value=BitConverter.UInt32BitsToSingle(U(at));Check(float.IsFinite(value)&&Math.Abs(value)<100000,"Non-finite serialized animation component");return value;}
    string Text(int at){Range(at,1);int end=System.Array.IndexOf(bytes,(byte)0,at);Check(end>=at&&end-at<256,"Invalid packfile string");return Encoding.UTF8.GetString(bytes,at,end-at);}
    int Pointer(int offset){Check(local.ContainsKey(offset),"Missing local pointer");Check(BitConverter.ToUInt64(bytes,Data+offset)==0,"Relocated pointer leaked into disk file");return local[offset];}
    int Reference(int offset){Check(global.TryGetValue(offset,out var p)&&p.Section==2,"Missing data-object reference");return p.At;}
    internal (int At,int Count) Array(int offset,int stride,int limit) {
        int count=I(Data+offset+8);uint capacity=U(Data+offset+12)&0x7fffffff;Check(count>=0&&count<=limit&&capacity>=count,"Invalid serialized array count/capacity");
        int at=count==0?0:Pointer(offset);Check((long)at+(long)count*stride<=sections[2].Local,"Serialized array crosses fixups");return(at,count);
    }
    Matrix4x4 Qs(int offset,bool output=false) {
        int at=Data+offset;var p=new Vector3(F(at),F(at+4),F(at+8));var q=new Quaternion(F(at+16),F(at+20),F(at+24),F(at+28));var scale=new Vector3(F(at+32),F(at+36),F(at+40));
        Check(Math.Abs(q.LengthSquared()-1)<.001f&&scale.X>0&&scale.Y>0&&scale.Z>0,"Invalid serialized QS transform");
        if(output)Check(F(at+12)==0&&F(at+44)==0,"Serialized QS SIMD padding is not canonical");
        return Matrix4x4.CreateScale(scale)*Matrix4x4.CreateFromQuaternion(q)*Matrix4x4.CreateTranslation(p);
    }
    internal sealed record Rig(string Name,string[] Names,int[] Parents,Matrix4x4[] Local);
    internal static Rig Skeleton(byte[] bytes) {
        var p=new RawPack(bytes);int obj=p.Objects["hkaSkeleton"];var names=p.Array(obj+40,16,256);var parents=p.Array(obj+24,2,256);var reference=p.Array(obj+56,48,256);
        Check(names.Count==parents.Count&&names.Count==reference.Count,"Native reference arrays disagree");
        return new(p.Text(p.Data+p.Pointer(obj+16)),Enumerable.Range(0,names.Count).Select(i=>p.Text(p.Data+p.Pointer(names.At+i*16))).ToArray(),Enumerable.Range(0,parents.Count).Select(i=>(int)unchecked((short)p.S(p.Data+parents.At+i*2))).ToArray(),Enumerable.Range(0,reference.Count).Select(i=>p.Qs(reference.At+i*48)).ToArray());
    }
    internal sealed record Clip(int Object,int Frames,int Bones,float Duration,Matrix4x4[][] Poses);
    internal (int Type,float Duration,int Tracks,int FloatTracks) Header(){int obj=Objects["hkaSplineCompressedAnimation"];return(I(Data+obj+12),F(Data+obj+16),I(Data+obj+20),I(Data+obj+24));}
    internal (int Frames,int MaximumFrames,float FrameDuration,float BlockDuration,float Inverse) Timing(){int obj=Objects["hkaSplineCompressedAnimation"];return(I(Data+obj+56),I(Data+obj+64),F(Data+obj+80),F(Data+obj+72),F(Data+obj+76));}
    internal string[] TrackNames(int animation) {
        var annotations=Array(animation+40,24,256);var names=new string[annotations.Count];
        for(int i=0;i<names.Length;++i){int at=annotations.At+i*24;names[i]=Text(Data+Pointer(at));Check(Array(at+8,16,256).Count==0,"Unexpected named annotation events");}
        return names;
    }
    internal sealed record Motion(Vector4 Up,Vector4 Forward,float Duration,Vector4[] Samples);
    internal Motion? ReferenceFrame(int animation) {
        if(!global.ContainsKey(animation+32))return null;int at=Reference(animation+32);Check(Objects["hkaDefaultAnimatedReferenceFrame"]==at,"Extracted motion points at the wrong native object");
        Vector4 V(int offset)=>new(F(Data+offset),F(Data+offset+4),F(Data+offset+8),F(Data+offset+12));
        var samples=Array(at+56,16,65536);
        Check(I(Data+at)==0&&I(Data+at+4)==0&&I(Data+at+8)==0&&I(Data+at+12)==0&&I(Data+at+52)==0&&I(Data+at+72)==0&&I(Data+at+76)==0,"Native reference-frame base/padding bytes differ from retail layout");
        return new(V(at+16),V(at+32),F(Data+at+48),Enumerable.Range(0,samples.Count).Select(i=>V(samples.At+i*16)).ToArray());
    }
    internal Clip Animation(int frames,int bones) {
        int obj=Objects["hkaSplineCompressedAnimation"];
        Check(I(Data+obj+12)==3&&I(Data+obj+20)==bones&&I(Data+obj+24)==0&&I(Data+obj+28)==0,"Wrong native animation type or track counts");
        Check(TrackNames(obj).Length==bones,"Native spline is missing its per-bone annotation names");
        Check(I(Data+obj+56)==frames&&I(Data+obj+60)==1&&I(Data+obj+64)==256&&I(Data+obj+68)==bones*4,"Wrong native spline block dimensions");
        float duration=F(Data+obj+16),delta=F(Data+obj+80);
        Check(Math.Abs(duration-delta*(frames-1))<.00001f&&Math.Abs(F(Data+obj+72)-255*delta)<.00001f&&Math.Abs(F(Data+obj+76)*F(Data+obj+72)-1)<.00001f,"Invalid native spline timing");
        var offsets=Array(obj+88,4,256);var floatOffsets=Array(obj+104,4,256);
        Check(offsets.Count==1&&I(Data+offsets.At)==0&&floatOffsets.Count==1&&Array(obj+120,4,256).Count==0&&Array(obj+136,4,256).Count==0&&I(Data+obj+168)==0,"Invalid native spline offset tables");
        var data=Array(obj+152,1,64000000);var raw=bytes.AsSpan(Data+data.At,data.Count).ToArray();
        using var reader=new BinaryReader(new MemoryStream(raw,false));
        void Align(){while(reader.BaseStream.Position%4!=0)Check(reader.ReadByte()==0,"Unexpected spline alignment bytes");}
        float Scalar(){float v=reader.ReadSingle();Check(float.IsFinite(v)&&Math.Abs(v)<100000,"Non-finite spline component");return v;}
        void Curve(bool align=true) {
            Check(reader.ReadUInt16()+1==frames&&reader.ReadByte()==1,"Native spline is not a linear sampled curve");
            Check(reader.ReadByte()==0,"Native spline curve missing initial clamp");
            for(int frame=0;frame<frames;++frame)Check(reader.ReadByte()==frame,"Native spline curve knot does not match its source frame");
            Check(reader.ReadByte()==frames-1,"Native spline curve missing final clamp");if(align)Align();
        }
        Vector3[] Vectors() {
            Curve();var min=new float[3];var max=new float[3];
            for(int axis=0;axis<3;++axis){min[axis]=Scalar();max[axis]=Scalar();Check(min[axis]<=max[axis],"Inverted native spline scalar bounds");}
            var values=new Vector3[frames];
            for(int frame=0;frame<frames;++frame)for(int axis=0;axis<3;++axis)values[frame][axis]=min[axis]+(max[axis]-min[axis])*reader.ReadUInt16()/65535f;
            return values;
        }
        var dynamicPosition=new bool[bones];var dynamicRotation=new bool[bones];var dynamicScale=new bool[bones];
        for(int bone=0;bone<bones;++bone) {
            Check(reader.ReadByte()==0x45,"Native spline does not use retail THREECOMP40 quantization");
            int position=reader.ReadByte(),rotation=reader.ReadByte();Check(position is 7 or 0x70&&rotation is 15 or 0xf0,"Native spline uses unexpected position/rotation masks");dynamicPosition[bone]=position==0x70;dynamicRotation[bone]=rotation==0xf0;
            int scale=reader.ReadByte();Check(scale is 7 or 0x70,"Invalid native spline scale mask");dynamicScale[bone]=scale==0x70;
        }
        Align();var poses=Enumerable.Range(0,frames).Select(_=>new Matrix4x4[bones]).ToArray();
        for(int bone=0;bone<bones;++bone) {
            var positions=dynamicPosition[bone]?Vectors():Enumerable.Repeat(new Vector3(Scalar(),Scalar(),Scalar()),frames).ToArray();Align();
            if(dynamicRotation[bone])Curve(false);var rotations=new Quaternion[frames];
            Quaternion Rotation() {
                ulong bits=0;for(int i=0;i<5;++i)bits|=(ulong)reader.ReadByte()<<(8*i);
                int largest=(int)((bits>>36)&3),component=0;var values=new float[4];
                for(int i=0;i<4;++i)if(i!=largest)values[i]=((int)((bits>>(12*component++))&4095)-2047)*.000345436f;
                values[largest]=MathF.Sqrt(Math.Max(0,1-values.Sum(v=>v*v)))*(((bits>>38)&1)==0?1:-1);
                return new(values[0],values[1],values[2],values[3]);
            }
            var firstRotation=Rotation();rotations[0]=firstRotation;
            for(int frame=1;frame<frames;++frame)rotations[frame]=dynamicRotation[bone]?Rotation():firstRotation;
            for(int frame=0;frame<frames;++frame){Check(Math.Abs(rotations[frame].LengthSquared()-1)<.00001f,"Native spline quaternion is not unit length");if(frame>0)Check(Quaternion.Dot(rotations[frame-1],rotations[frame])>=0,"Native spline quaternion signs cross hemispheres");}
            Align();Vector3[] scales;
            if(dynamicScale[bone])scales=Vectors();else{var scale=new Vector3(Scalar(),Scalar(),Scalar());scales=Enumerable.Repeat(scale,frames).ToArray();}
            Align();
            for(int frame=0;frame<frames;++frame){var s=scales[frame];Check(s.X>0&&s.Y>0&&s.Z>0,"Invalid native spline scale");poses[frame][bone]=Matrix4x4.CreateScale(s)*Matrix4x4.CreateFromQuaternion(rotations[frame])*Matrix4x4.CreateTranslation(positions[frame]);}
        }
        Check(reader.BaseStream.Position==I(Data+floatOffsets.At),"Native transform block endpoint differs from float-block offset");
        while(reader.BaseStream.Position%16!=0)Check(reader.ReadByte()==0,"Unexpected native spline block padding");
        Check(reader.BaseStream.Position==raw.Length,"Native spline block contains unexpected trailing bytes");
        bool animated=poses.Skip(1).Any(p=>!p.SequenceEqual(poses[0]));var motion=ReferenceFrame(obj);
        if(!animated)Check(dynamicPosition.All(v=>!v)&&dynamicRotation.All(v=>!v),"Native static pose contains unnecessary position/rotation splines");
        Check((motion!=null)==animated,"Native reference-frame presence does not match clip motion");
        if(motion!=null)Check(motion.Up==new Vector4(0,1,0,0)&&motion.Forward==new Vector4(0,0,1,0)&&motion.Duration==duration&&motion.Samples.Length==frames&&motion.Samples.All(v=>v==Vector4.Zero),"Native reference-frame fields or zero actor-motion samples changed");
        return new(obj,frames,bones,duration,poses);
    }
    internal Matrix4x4[] Pose(Clip clip,int frame)=>clip.Poses[frame];
    internal Matrix4x4[] FractionalPose(Clip clip,float frame) {
        int first=(int)MathF.Floor(frame),last=Math.Min(first+1,clip.Frames-1);float ratio=frame-first;var result=new Matrix4x4[clip.Bones];
        for(int bone=0;bone<result.Length;++bone) {
            Check(Matrix4x4.Decompose(clip.Poses[first][bone],out var sa,out var qa,out var pa)&&Matrix4x4.Decompose(clip.Poses[last][bone],out _,out _,out _),"Cannot decompose decoded fixture spline");
            Matrix4x4.Decompose(clip.Poses[last][bone],out var sb,out var qb,out var pb);if(Quaternion.Dot(qa,qb)<0)qb=-qb;
            var q=Quaternion.Normalize(qa*(1-ratio)+qb*ratio);result[bone]=Matrix4x4.CreateScale(Vector3.Lerp(sa,sb,ratio))*Matrix4x4.CreateFromQuaternion(q)*Matrix4x4.CreateTranslation(Vector3.Lerp(pa,pb,ratio));
        }
        return result;
    }
    internal bool Binding(string skeletonName,int animation,int bones) {
        int b=Objects["hkaAnimationBinding"];var map=Array(b+32,2,256);
        Check(Text(Data+Pointer(b+16))==skeletonName&&Reference(b+24)==animation&&map.Count==bones&&bytes[Data+b+80]==0,"Invalid native animation binding");
        for(int i=0;i<bones;++i)Check(S(Data+map.At+i*2)==i,"Native animation bindings are not identity bone order");
        Check(Array(b+48,2,256).Count==0&&Array(b+64,2,256).Count==0,"Unexpected native float or partition bindings");
        int container=Objects["hkaAnimationContainer"];Check(Array(container+16,8,256).Count==0,"Clip embedded skeletons");
        var animations=Array(container+32,8,16);var bindings=Array(container+48,8,16);
        Check(animations.Count==1&&bindings.Count==1&&Reference(animations.At)==animation&&Reference(bindings.At)==b,"Container points at wrong animation/binding objects");return true;
    }
}
