// SPDX-License-Identifier: GPL-3.0-or-later
#nullable enable
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text.Json;
using Spline=StudioCore.Utilities.SplineCompressedAnimation;

static class EnemyProportionsTests {
    static void Check(bool ok,string message){if(!ok)throw new Exception(message);}
    static void Reject(Action action,string message){try{action();}catch(InvalidDataException){return;}throw new Exception(message);}
    static string Leaf(string path)=>path.Replace('\\','/').Split('/').Last();
    static List<Spline.TransformTrack[]> Decode(byte[] bytes) {
        var p=new NativeAnimation.Pack(bytes);int a=p.Object("hkaSplineCompressedAnimation");var data=p.Array(a+152,1,64*1024*1024);
        return Spline.ReadSplineCompressedAnimByteBlock(false,bytes.AsSpan(p.Data+data.At,data.Count).ToArray(),p.Int(a+20),p.Int(a+60));
    }
    static (Vector3 Position,Quaternion Rotation,Vector3 Scale) Sample(Spline.TransformTrack t,float frame) {
        Vector3 p=t.StaticPosition,s=t.StaticScale;
        if(t.HasSplinePosition)p=new(t.SplinePosition.GetValueX(frame)??p.X,t.SplinePosition.GetValueY(frame)??p.Y,t.SplinePosition.GetValueZ(frame)??p.Z);
        if(t.HasSplineScale)s=new(t.SplineScale.GetValueX(frame)??s.X,t.SplineScale.GetValueY(frame)??s.Y,t.SplineScale.GetValueZ(frame)??s.Z);
        return(p,t.HasSplineRotation?t.SplineRotation.GetValue(frame):t.StaticRotation,s);
    }
    public static void Main(string[] args) {
        string source=args[0],native=args[1];var hashes=new Dictionary<string,byte[]>();
        byte[] Read(string path){var b=File.ReadAllBytes(path);hashes.TryAdd(path,SHA256.HashData(b));return b;}
        var character=BND4.Read(Read(Path.Combine(native,"chr/c2610.chrbnd.dcx")));
        var model=character.Files.Single(f=>Leaf(f.Name).EndsWith(".flver")).Bytes.ToArray();var originalNodes=FLVER2.Read(model).Nodes;
        var animation=BND4.Read(Read(Path.Combine(native,"chr/c2610.anibnd.dcx")));var skeleton=animation.Files.Single(f=>f.ID==1000000).Bytes.ToArray();
        var clips=animation.Files.Where(f=>f.ID>=1000000000).ToArray();Check(clips.Length==328,"Native hollow animation population changed");
        var poses=new List<object>();int checkedFrames=0,multiblocks=0;
        foreach(string name in new[]{"c2500","c2540","c2550","c2560"}) {
            var bnd=BND3.Read(Read(Path.Combine(source,"chr",name+".chrbnd.dcx")));
            var sourceModel=bnd.Files.Single(f=>Leaf(f.Name).EndsWith(".flver")).Bytes.ToArray();
            var plan=EnemyProportions.Create(sourceModel,model,skeleton);var rig=NativeAnimation.Skeleton(plan.Skeleton);
            if(name=="c2500") {
                byte[] first=clips[0].Bytes.ToArray();var pack=new NativeAnimation.Pack(first);int anim=pack.Object("hkaSplineCompressedAnimation"),binding=pack.Object("hkaAnimationBinding");var data=pack.Array(anim+152,1,64*1024*1024);int start=pack.Data+data.At;
                void Bad(int offset,byte[] bytes,string reason){var bad=first.ToArray();bytes.CopyTo(bad,offset);Reject(()=>EnemyProportions.Clip(bad,plan),reason);}
                Bad(start,[0x44],"Unsupported spline quantization was accepted");
                Bad(pack.Data+anim+24,BitConverter.GetBytes(1),"Unexpected float tracks were accepted");
                Bad(pack.Data+binding+80,[2],"Unsupported animation blend type was accepted");
                var map=pack.Array(binding+32,2,256);Bad(pack.Data+map.At+2,BitConverter.GetBytes((short)0),"Nonidentity bone bindings were accepted");
                int degree=first[start+320+2];Bad(start+320+3+degree,[1],"Invalid spline knot domain was accepted");
            }
            var nodes=FLVER2.Read(plan.Model).Nodes;var world=SkinRetarget.World(nodes);
            for(int i=0;i<rig.Names.Length;i++) {
                int node=nodes.FindIndex(n=>n.Name==rig.Names[i]);Check(node>=0&&Vector3.Distance(nodes[node].Translation,originalNodes[node].Translation*plan.ModelFactors[i])<.00001f,"FLVER local translation lost the source length ratio");
                Check(nodes[node].Rotation==originalNodes[node].Rotation&&nodes[node].Scale==originalNodes[node].Scale,"FLVER bind rotation or scale changed");
                Check(Vector3.Distance(rig.Local[i].Translation,plan.Original.Local[i].Translation*plan.Factors[i])<.00001f,"Reference translation changed incorrectly");
                var a=rig.Local[i];var b=plan.Original.Local[i];a.M41=b.M41=0;a.M42=b.M42=0;a.M43=b.M43=0;Check(a==b,"Reference rotation or scale changed");
            }
            foreach(var segment in plan.Segments) {
                Check(Math.Abs(segment.NativeLength*segment.Factor-segment.SourceLength)<.00001f,"Source bone length was not retained: "+segment.Bone);
                int at=nodes.FindIndex(n=>n.Name==segment.Bone),stop=nodes.FindIndex(n=>n.Name==segment.Ancestor);float length=0;
                while(at!=stop&&at>=0){int parent=nodes[at].ParentIndex;length+=Vector3.Distance(world[at].Translation,parent<0?Vector3.Zero:world[parent].Translation);at=parent;}
                Check(Math.Abs(length-segment.SourceLength)<.002f,$"Corrected model segment differs from source: {segment.Bone} {length}/{segment.SourceLength}");
            }
            foreach(var clip in clips) {
                byte[] before=clip.Bytes.ToArray(),after=EnemyProportions.Clip(before,plan);Check(before.Length==after.Length&&before.AsSpan().SequenceEqual(clip.Bytes.Span),"Clip structure or immutable input changed");
                var p=new NativeAnimation.Pack(before);int a=p.Object("hkaSplineCompressedAnimation");var data=p.Array(a+152,1,64*1024*1024);
                Check(before.AsSpan(0,p.Data+data.At).SequenceEqual(after.AsSpan(0,p.Data+data.At))&&before.AsSpan(p.Data+data.At+data.Count).SequenceEqual(after.AsSpan(p.Data+data.At+data.Count)),"Clip timing/binding/extracted-motion/fixup data changed");
                var old=Decode(before);var following=Decode(after);Check(old.Count==following.Count,"Block count changed");if(old.Count>1)multiblocks++;
                for(int block=0;block<old.Count;block++) {
                    float end=Math.Min(255,p.Int(a+56)-1-block*255);
                    foreach(float frame in new[]{0f,end*.5f,end}) {
                        var local=new Matrix4x4[rig.Names.Length];var oldLocal=new Matrix4x4[rig.Names.Length];
                        for(int bone=0;bone<rig.Names.Length;bone++) {
                            var x=Sample(old[block][bone],frame);var y=Sample(following[block][bone],frame);
                            Check(Vector3.Distance(x.Position*plan.Factors[bone],y.Position)<.00002f,"Animated positional channel lost source ratio: "+clip.ID+"/"+bone);
                            Check(x.Rotation==y.Rotation&&x.Scale==y.Scale,"Native rotation/scale changed: "+clip.ID+"/"+bone);
                            local[bone]=Matrix4x4.CreateScale(y.Scale)*Matrix4x4.CreateFromQuaternion(y.Rotation)*Matrix4x4.CreateTranslation(y.Position);
                            oldLocal[bone]=Matrix4x4.CreateScale(x.Scale)*Matrix4x4.CreateFromQuaternion(x.Rotation)*Matrix4x4.CreateTranslation(x.Position);
                        }
                        var posed=NativeAnimation.Worlds(local,rig.Parents);Check(posed.All(m=>float.IsFinite(m.M41)&&float.IsFinite(m.M42)&&float.IsFinite(m.M43)),"Nonfinite corrected native pose");checkedFrames++;
                        if(clip.ID==1000000000&&block==0&&frame==end*.5f) {
                            float Chain(Matrix4x4[] matrices,string first,string last) {int at=Array.IndexOf(rig.Names,last),stop=Array.IndexOf(rig.Names,first);float length=0;while(at!=stop){int parent=rig.Parents[at];Check(parent>=0,"Not an anatomical chain");length+=Vector3.Distance(matrices[at].Translation,matrices[parent].Translation);at=parent;}return length;}
                            var oldWorld=NativeAnimation.Worlds(oldLocal,rig.Parents);
                            object Shape(Matrix4x4[] matrices)=>new {neck=Chain(matrices,"Neck","Head"),upperArm=Chain(matrices,"L UpperArm","L Forearm"),forearm=Chain(matrices,"L Forearm","L Hand")};
                            poses.Add(new {model=name,clip=clip.ID,frame,before=Shape(oldWorld),after=Shape(posed),segments=plan.Segments});
                        }
                    }
                }
            }
            Console.WriteLine($"PASS: {name}: {plan.Segments.Count} source bone lengths; all328 native clips retain rotations/scales/timing across independent decoded poses");
        }
        foreach(var (path,hash) in hashes)Check(SHA256.HashData(File.ReadAllBytes(path)).AsSpan().SequenceEqual(hash),"Original asset changed: "+path);
        if(args.Length>2){Directory.CreateDirectory(args[2]);File.WriteAllText(Path.Combine(args[2],"pose-comparison.json"),JsonSerializer.Serialize(new{checkedFrames,multiblocks,originals=hashes.Keys,poses},new JsonSerializerOptions{WriteIndented=true,IncludeFields=true}));}
        Console.WriteLine($"PASS: {checkedFrames} independent posed frames, {multiblocks} multi-block conversions, {hashes.Count} original asset hashes unchanged");
    }
}
